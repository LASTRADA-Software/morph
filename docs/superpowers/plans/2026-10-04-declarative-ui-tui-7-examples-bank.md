# Declarative UI, Part 7 — `examples/bank`: One App, Any Frontend Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for
> tracking.

**Goal:** Replace the bank's QML GUI, its seven QObject controllers and its WebAssembly copy with `bank_app` — a
toolkit-free library of six declarative controllers and their `morph::ui` views — plus one binary, `bank`, that
runs on the terminal or Qt Quick (native) and on Qt Quick alone (WebAssembly, over the in-memory models).

**Architecture:** Each screen is a controller (a `Store` of user intent, `Query`s keyed on the signed-in state and
the selection, `Mutation`s that invalidate those queries, `Computed` projections that hold every format and
validation) and a view function that only binds. `BankApplication` owns the controllers, a shared `Notices` strip
and the route; `bank::client::makeApplication(ctx, env)` connects through `examples::connect`. The client lives in
`bank::client`, because `bank::app` is the domain library's Qt-free `App` (`include/bank/app/app.hpp`); the directory
`examples/bank/app/` and the target `bank_app` keep their names. `ui/main.cpp` is the composition root of spec 4 §3;
the browser build compiles the same sources against the shadow models in `examples/bank/wasm/`.

**Tech Stack:** C++23; `morph::reactive` (Part 1), `morph::ui` and `ui::testing::RecordingBackend` (Part 2),
`morph::tui` (Part 3), `morph::qt_quick` (Part 4), `examples/common`'s app layer, `FakeAppContext`, test waits and
smoke harness (Part 6); bank's
`bank_lib` (Lightweight ORM over SQLite/ODBC); Catch2 v3; Emscripten + Qt for WebAssembly.

**Spec:** `docs/superpowers/specs/2026-10-04-examples-migration-design.md` (spec 4) §1, §2, §3, §5 (bank), §6, §7,
§8, §9; `docs/superpowers/specs/2026-10-04-declarative-ui-tui-design.md` (spec 1) §4b (controllers, and the
`TransactionController` A/B stale-selection race).

This is **Part 7 of 11** of the declarative-UI program. Read the master plan
`docs/superpowers/plans/2026-10-04-declarative-ui-tui.md` first: it fixes the branch, the commit layout and the
`wip(...)` commit convention every task below follows, and this part ends by squashing its `wip(bank)` commits
into one commit, `examples/bank: one app, any frontend`. Parts 1–6 have landed.

## Global Constraints

- Part 1's constraints apply unchanged (SPDX line first, `#pragma once`, clang-tidy naming, present-tense comments
  with no history or issue numbers, sign-off trailer), except the two that concern `include/morph` only: bank code
  is not in the Doxygen input (`docs/CMakeLists.txt` reads `include/morph` alone) and is not strict-warning code.
- `bank_app`, like `bank_lib`, is compiled **without** `apply_warnings()`: its controllers include the model
  headers, which include the Lightweight ORM's, and those are not `-Werror` clean. clang-tidy-diff still applies.
- `bank_app` links `morph::morph`, `bank_lib` and `morph::ladder_app_common` and nothing else: no Qt, no
  `morph::tui`, no `morph::qt_quick`. A configure-time check of its link set and `app/toolkit_free.cpp`'s
  `#error` enforce it (spec 4 §7 rule 1).
- Controllers include nothing from `morph::ui`; a view function includes `morph/ui/view.hpp`, its controller and
  `views/style.hpp`, and holds no conditional, format or validation (spec 4 §7 rule 2). `views/style.hpp`'s four
  adapters (tone → role, choices → options, id ↔ key) are the only translation, because a controller cannot name
  a `ui` type.
- A controller's members are declared in this order: borrowed pointers, `Store`, `Query`s (a `Computed` a later
  query's key reads sits directly before that query, because the query's effect reads it in the constructor),
  `Mutation`s, `Computed` projections, `FailureReporter`, and a `CallbackScope` last.
- Every failure reaches the user through the shared `Notices` (`reactive::errorMessage`), never through a
  per-controller error string; every confirmation (`Transaction posted`, `Bill paid`) too.
- A button whose input is invalid is disabled (`common.enabled` bound to the controller's `can…()` Computed), and
  the controller's command refuses the same input (spec 1 §4b `action(make)`): the two agree by construction.
- Tests never sleep: they wait through `settle()` / `drain()` / `awaitReply()` (Task 1), which are
  `examples::testing::pumpUntil` / `awaitOn` over Part 6's `FakeAppContext`'s `MainThreadExecutor` with a 10 s
  budget.
- Client code lives in `namespace bank::client` (`bank::client::views` for the views); `bank::app` is the domain
  library's `App`. Parameters and locals are at least three characters long (clang-tidy
  `readability-identifier-length`; only `i j k x y n N fn cb op` and `lk` are exempt).
- Every bank target carries the `if(DEFINED AF_SANITIZER) apply_sanitizers(...)` block: an instrumented `bank_lib`
  with an uninstrumented executable does not link (`examples/bank/CMakeLists.txt`'s own measurement).
- Bank lists its sources and test sources by hand in `examples/bank/CMakeLists.txt`; nothing is globbed.
- Example-only change: no `CHANGELOG.md` entry, no `docs/spec/` file (CONTRIBUTING, "The changelog").

## Review Focus

1. **A late history reply for the previously selected account** must not replace the history of the account now
   selected — the defect spec 1 §4b names (Task 4 test "a late reply for the previously selected account does not
   replace the shown history").
2. **The refresh a deposit causes** must keep the user's selection, so the next deposit lands in the account the
   picker names — the property the old live-engine QML test held (Task 4 test "the selection survives the refresh
   a deposit causes, and the next deposit lands in the shown account").
3. **Signing out and in again as the same user** produces a reply identical to the previous one; adoption must
   not depend on the reply differing (Task 2 test "signing out and in again as the same user signs in").
4. **An amount outside the int64 minor-unit range, `inf` or `nan`** must be rejected before it is rounded —
   rounding it is undefined behaviour (Task 1 test "parseMinor rejects amounts that do not fit in int64 minor
   units").
5. **Signing out** must leave nothing of the previous user on screen: every query keyed on the session goes idle
   and clears (Task 3 test "signing out empties the list and the total returns to —").

---

## File Structure

| File | Responsibility |
|---|---|
| `examples/bank/app/tone.hpp` | `Tone` — status/amount emphasis without toolkit colours |
| `examples/bank/app/choice.hpp` | `Choice` (id + label for a picker), `resolveChoice` (the user's pick, else the first row) |
| `examples/bank/app/format.hpp` | `fmt::` — `money`, `accountKind`, `txnKind`, `last4`, `trimmed`, `parseMinor`, `parseWhole` |
| `examples/bank/app/notices.hpp`, `.cpp` | `Notice`, `Notices` (the shared strip, hidden after 3.2 s), `FailureReporter`, `confirmed()` |
| `examples/bank/app/bank_handlers.hpp`, `.cpp` | `BankHandlers` — one `BridgeHandler` per model the client calls |
| `examples/bank/app/local_setup.hpp`, `.cpp` | `databaseConnection(--db)`, `localSetup()` (native: migrate the SQLite database) |
| `examples/bank/app/toolkit_free.cpp` | The compile-time guard: a Qt module on `bank_app`'s compile line is an `#error` |
| `examples/bank/app/account_choices.hpp` | `openAccountChoices()` — the open accounts as picker choices |
| `examples/bank/app/controllers/auth_controller.hpp`, `.cpp` | `login::` Store, `AuthController` |
| `examples/bank/app/controllers/accounts_controller.hpp`, `.cpp` | `opening::` Store, `AccountCard`, `AccountsController` |
| `examples/bank/app/controllers/transactions_controller.hpp`, `.cpp` | `movemoney::` Store, `HistoryRow`, `HistoryFetch`, `TransactionsController` |
| `examples/bank/app/controllers/cards_controller.hpp`, `.cpp` | `issuing::` Store, `CardRow`, `CardsController` |
| `examples/bank/app/controllers/payees_controller.hpp`, `.cpp` | `payee::` Store, `PayeeRow`, `PayeesController` |
| `examples/bank/app/controllers/loans_controller.hpp`, `.cpp` | `lending::` Store, `LoanRow`, `InstallmentRow`, `LoansController` |
| `examples/bank/app/views/style.hpp` | `roleOf`, `optionsOf`, `keyOf`, `accountKeyOf`, `idOf` |
| `examples/bank/app/views/*_view.hpp`, `.cpp` | `loginView`, `accountsView`, `moveMoneyView`, `cardsView`, `payeesView`, `loansView`, `shellView`, `appView`, `noticeView` |
| `examples/bank/app/demo_seed.hpp`, `.cpp` | `DemoSeed` — signs in as `demo`, creating it with two EUR accounts on first use |
| `examples/bank/app/bank_application.hpp`, `.cpp` | `Route`, `Screen`, `DemoData`, `BankApplication`, `makeApplication` |
| `examples/bank/ui/main.cpp` | The composition root (spec 4 §3), native and WebAssembly |
| `examples/bank/wasm/` (moved from `gui_wasm/`) | Shadow model headers, `bank::wasm` store, in-memory models, `local_setup_wasm.cpp`, the browser target |
| `examples/bank/tests/app/test_app_context.hpp` | `settle`, `drain`, `awaitReply` over Part 6's `FakeAppContext` (no Lightweight) |
| `examples/bank/tests/app/bank_app_test_support.hpp` | `localEnvironment`, `signUp`, `BankWiring` (a local bridge, handlers, notices, auth) |
| `examples/bank/tests/app/recorded.hpp` | `RecordingBackend` name helpers, `mount`, `checkGolden` |
| `examples/bank/tests/app/test_*.cpp`, `golden/*.txt` | Controller and view tests per screen; tree-dump goldens |
| `examples/bank/tests/inmemory/test_inmemory_models.cpp` | The app over the WebAssembly model set, natively |
| `examples/bank/tests/smoke/test_bank_frontend_smoke.cpp` | `bank_smoke_tests`, a binary of its own: mounts and quits on each built frontend |
| `examples/bank/CMakeLists.txt` | `BANK_APP_SOURCES`, `bank_app`, `bank`, the three new test binaries, the WASM branch |
| Removed: `examples/bank/gui/`, `examples/bank/tests/gui/`, `MORPH_BUILD_BANK_GUI` | The QML client, its tests and its option |
| `CMakeLists.txt`, `CMakePresets.json`, `.github/workflows/{ci,nightly-slow-checks,wasm-demo}.yml`, `scripts/check_coverage_objects.sh` | Option and target renames |
| `examples/bank/README.md`, `examples/TESTING.md`, `examples/LADDER.md`, `README.md` | The client section and every sentence the removal falsifies |

## Build and test commands (used by every task)

```bash
cmake -S . -B build/bank -G Ninja -DCMAKE_BUILD_TYPE=Debug -DMORPH_BUILD_BANK_EXAMPLE=ON \
      -DMORPH_BUILD_TUI=ON -DMORPH_BUILD_QT=ON -DMORPH_BUILD_QT_QUICK=ON -DMORPH_BUILD_NET=ON   # once
cmake --build build/bank --target bank_app_tests
./build/bank/examples/bank/bank_app_tests "[bank][app]"
```

The bank example needs unixODBC and the SQLite3 ODBC driver (`examples/bank/README.md`, "Build & run").
Configuring prints `morph: warnings: ... strict=ON`; if it does not, stop and say so (CONTRIBUTING).

### What this plan takes from Part 6 and Part 2

Part 6 (`2026-10-04-declarative-ui-tui-6-examples-foundation.md`) defines every name below; Task 1 Step 0 confirms
them in the tree before anything is built on them.

1. Target `morph_ladder_app_common` / `morph::ladder_app_common` (Part 6 Task 2), in `examples/common/app/`, added
   once by the root `CMakeLists.txt` whenever the ladder, the bank example or `examples/tui` is on. Its interface is
   Qt-free (the Qt and `morph::net` transports are PRIVATE), and its PUBLIC include root is `examples/common`, so
   headers are `<app/…>` and `<testkit/…>`. Bank links it by name, never behind `if(TARGET …)`.
2. `app/app_environment.hpp`: `AppEnvironment{server, db, user, seed, pollId}`, `fromArgs(argc, argv)`. `db` is
   empty by default, and bank maps empty to its own default file in the temp directory
   (`databaseConnection("")`). A non-empty `user` becomes the bridge's default session (`connect` installs it).
3. `app/transport.hpp`: `TransportError`, `LocalSetup{setupDatabase, workers}`, `Link{Local, Remote}`,
   `Connection{bridge(), callbacks(), ready(), link()}` and `connect(ctx, env, LocalSetup)`. `ready()` is a tracked
   `Signal<bool>`, true at once for a local link and true for a remote one only while its transport is up; bank's
   demo seed waits for it (Task 8). `app/uuid.hpp`: `newUuid()`.
4. Header-only `testkit/wait.hpp`: `pumpUntil(exec::MainThreadExecutor&, Pred, budget = 2000ms)` and
   `awaitOn(exec::MainThreadExecutor&, Completion<T>, budget = 2000ms) -> T`, budgets scaled by
   `MORPH_LADDER_DEADLINE_MS`. Header-only `testkit/fake_app_context.hpp`: `FakeAppContext(frontendName = "test",
   ioLoop = nullptr)` with `owner()`, `manualScheduler()`, `quitCode()`. `bank_app_tests` and `bank_inmemory_tests`
   reach both through `morph::ladder_app_common`'s include root and link no frontend.
5. The smoke harness `testkit/frontend_smoke.hpp` (`SmokeFrontend`, `smokeRun`, `runFrontendSmoke`, which SKIPs a
   frontend this configure did not build) is compiled into the STATIC target `morph_example_testkit` /
   `morph::example_testkit`, which links every built frontend PUBLIC and exists when `MORPH_BUILD_TESTS` is on
   outside Emscripten. `qt_quick::Frontend::run` constructs its own `QGuiApplication`, so smoke tests run from a
   binary of their own whose `main` is `morph_test_main` — for bank, `bank_smoke_tests` (Task 9).
6. CMake `morph_example_frontends(<target> <scope>)` (`cmake/morph_example_app.cmake`) links each built frontend and
   defines `MORPH_EXAMPLE_HAS_TUI` / `MORPH_EXAMPLE_HAS_QT_QUICK` to 0 or 1. The `bank` binary uses it directly
   rather than `morph_add_example_ui`, because that helper calls `apply_warnings()` and bank's targets cannot: the
   model headers include the Lightweight ORM's, which are not `-Werror` clean.

Part 2's `RecordingBackend` is read through `tests/app/recorded.hpp`, in Part 2's spellings: kinds `Text`, `Button`,
`TextInput`, `Select`, `Menu` (a stack is `Column` or `Row` by its axis, a `Switch` is a `Slot`, a vertical
`forEach` a `Column`, a table a `Table` of `Row`s); properties `text`, `label`, `placeholder`, `enabled`,
`visible`; booleans print as `true`/`false`, and `enabled`/`visible` are set only when bound or `false`;
`chooseIndex` activates a `Menu` entry, and a hidden or disabled widget ignores every interaction helper. Task 2
Step 0 confirms them against the header.

---

### Task 1: `bank_app` — formatting, notices, handlers, the local database and the toolkit-free guard

**Files:**
- Create: `examples/bank/app/tone.hpp`, `examples/bank/app/choice.hpp`, `examples/bank/app/format.hpp`
- Create: `examples/bank/app/notices.hpp`, `examples/bank/app/notices.cpp`
- Create: `examples/bank/app/bank_handlers.hpp`, `examples/bank/app/bank_handlers.cpp`
- Create: `examples/bank/app/local_setup.hpp`, `examples/bank/app/local_setup.cpp`,
  `examples/bank/app/toolkit_free.cpp`
- Create: `examples/bank/tests/app/test_app_context.hpp`
- Modify: `examples/bank/CMakeLists.txt` — three places: (a) directly after the `if(NOT TARGET morph::morph) … endif()`
  block and before `if(EMSCRIPTEN)`, the app-layer subdirectory and `BANK_APP_SOURCES`; (b) after the
  `# ── CLI driver` block (ending `apply_sanitizers(bank_cli …)` / `endif()`), the `bank_app` library; (c) inside
  `if(MORPH_BUILD_TESTS)` … `else()`, directly after `catch_discover_tests(bank_tests … PROPERTIES LABELS "bank")`,
  the `bank_app_tests` executable
- Test: `examples/bank/tests/app/test_format.cpp`, `examples/bank/tests/app/test_notices.cpp`

**Interfaces:**
- Consumes: `morph::reactive::{Runtime, Signal, Effect, Scheduler, TimerHandle, errorMessage}` and
  `reactive::testing::ManualScheduler{advance, pendingTimers}` (Part 1); `morph::ui::{AppContext, Scheduler}`
  (Part 2, `ui/frontend.hpp`); `morph::examples::LocalSetup{setupDatabase, workers}` (`app/transport.hpp`),
  `morph::examples::testing::pumpUntil(exec::MainThreadExecutor&, Pred, std::chrono::milliseconds)` and
  `awaitOn(exec::MainThreadExecutor&, async::Completion<T>, std::chrono::milliseconds) -> T` (`testkit/wait.hpp`),
  `morph::examples::testing::FakeAppContext{owner(), manualScheduler(), quitCode()}`
  (`testkit/fake_app_context.hpp`) (Part 6); `bank::format(Money)`, `bank::pow10i`, `bank::AccountKind`, `bank::TxnKind`
  (`include/bank/core/{money,types}.hpp`); `bank::db::setup(std::string const&)` (`include/bank/db/database.hpp`);
  `morph::bridge::BridgeHandler<M, S>::execute`, `async::Completion<T>::then(CallbackScope const&, …)`
  (`include/morph/core/{bridge,completion}.hpp`).
- Produces (every later task uses these exact names, all in `namespace bank::client`):
  - `enum class Tone : std::uint8_t { Neutral, Good, Warn, Bad }`.
  - `struct Choice { std::int64_t id; std::string label; }` (equality-comparable);
    `template <class Row> std::int64_t resolveChoice(std::vector<Row> const&, std::int64_t chosen)`.
  - `fmt::trimmed(std::string_view)`, `fmt::money(std::int64_t, int)`, `fmt::accountKind(int)`, `fmt::txnKind(int)`,
    `fmt::last4(std::string_view)`, `fmt::kMinorUnitsBound`, `fmt::parseMinor(std::string_view, int = 2) ->
    std::optional<std::int64_t>`, `fmt::parseWhole(std::string_view) -> std::optional<int>`.
  - `struct Notice { std::string text; Tone tone; }`; `class Notices { static constexpr kShownFor; Notices(Runtime&,
    Scheduler&); error(std::string); success(std::string); dismiss(); visible() -> bool; text() -> std::string
    const&; tone() -> Tone; }` (reads tracked).
  - `class FailureReporter { using Source = std::function<std::exception_ptr()>; FailureReporter(Runtime&, Notices&,
    std::vector<Source>); }`.
  - `template <class Action, class M, class S> confirmed(BridgeHandler<M, S>&, Notices&, async::CallbackScope
    const&, std::string text) -> std::function<async::Completion<Result>(Action)>`.
  - `struct BankHandlers { BankHandlers(bridge::Bridge&, exec::IExecutor&); auth, customers, transactions, cards,
    payees, payments, loans; }`.
  - `databaseConnection(std::string_view) -> std::string`, `localSetup() -> morph::examples::LocalSetup`.
  - Test support (`namespace bank::testing`): `using morph::examples::testing::FakeAppContext`, `kSettleBudget`,
    `settle(FakeAppContext&, Pred) -> bool`, `drain(FakeAppContext&)`, `awaitReply(FakeAppContext&,
    async::Completion<T>) -> T`.
  - CMake: `BANK_APP_SOURCES` (absolute paths), targets `bank_app`, `bank_app_tests`, definition `BANK_GOLDEN_DIR`.

- [ ] **Step 0: Confirm what this part takes from Part 6**

```bash
grep -n 'add_library(morph_ladder_app_common\|add_library(morph::ladder_app_common\|add_library(morph_example_testkit' \
     examples/common/app/CMakeLists.txt
grep -n 'target_link_libraries(morph_ladder_app_common' examples/common/app/CMakeLists.txt
grep -n 'struct AppEnvironment\|struct LocalSetup\|enum class Link\|ready() const\|connect(ui::AppContext' \
     examples/common/app/app_environment.hpp examples/common/app/transport.hpp
grep -n 'newUuid' examples/common/app/uuid.hpp
grep -n 'bool pumpUntil(exec::MainThreadExecutor\|T awaitOn(' examples/common/testkit/wait.hpp
grep -n 'class FakeAppContext\|manualScheduler()\|quitCode()' examples/common/testkit/fake_app_context.hpp
grep -n 'void runFrontendSmoke\|SmokeResult smokeRun' examples/common/testkit/frontend_smoke.hpp
grep -n 'function(morph_example_frontends' cmake/morph_example_app.cmake
```

Expected: the first `grep` prints the three `add_library` lines; the second prints `PUBLIC morph::morph`,
`PRIVATE morph::qt morph_qt_impl Qt6::Core` and `PRIVATE morph::net` (the transports are PRIVATE, so no Qt compile
definition or include reaches `bank_app`); every other `grep` prints each name it names.
These are the names "What this plan takes from Part 6 and Part 2" lists; a missing one means Part 6 has not landed
as planned — stop and say so rather than substituting.

- [ ] **Step 1: Write the failing tests**

Create `examples/bank/tests/app/test_app_context.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <memory>
#include <morph/core/completion.hpp>
#include <testkit/fake_app_context.hpp>
#include <testkit/wait.hpp>
#include <utility>

/// @file
/// The bank's waits over Part 6's headless `FakeAppContext`, with the budget the bank's models need.
/// Nothing here includes Lightweight, so the in-memory model tests use it as well.

namespace bank::testing {

/// @brief The headless `ui::AppContext` every bank controller and view test builds on: a `MainThreadExecutor`
///        the test pumps (`owner()`), the `Runtime` on it, and a `ManualScheduler` (`manualScheduler()`).
using morph::examples::testing::FakeAppContext;

/// @brief How long a test waits for the bank's models before it fails.
inline constexpr std::chrono::milliseconds kSettleBudget{10000};

/// @brief Pumps the owner until @p done holds or `kSettleBudget` runs out.
/// @tparam Pred A predicate taking no arguments.
/// @param ctx The test's context.
/// @param done The condition to wait for.
/// @return Whether @p done held in time.
template <typename Pred>
[[nodiscard]] bool settle(FakeAppContext& ctx, Pred done) {
    return morph::examples::testing::pumpUntil(ctx.owner(), std::move(done), kSettleBudget);
}

/// @brief Runs everything already posted to the owner, and the flush it requests, then returns.
/// @param ctx The test's context.
inline void drain(FakeAppContext& ctx) {
    auto const reached = std::make_shared<bool>(false);
    ctx.owner().post([reached] { *reached = true; });
    REQUIRE(settle(ctx, [&ctx, reached] { return *reached && !ctx.runtime().isFlushRequested(); }));
}

/// @brief Waits for @p reply by pumping the owner, and returns its value.
/// @tparam T The reply's type.
/// @param ctx The test's context; @p reply must deliver on its owner.
/// @param reply A call made behind every controller's back.
/// @return The value; rethrows the call's failure, and throws when `kSettleBudget` runs out.
template <typename T>
T awaitReply(FakeAppContext& ctx, morph::async::Completion<T> reply) {
    return morph::examples::testing::awaitOn(ctx.owner(), std::move(reply), kSettleBudget);
}

}  // namespace bank::testing
```

Create `examples/bank/tests/app/test_format.cpp` — `Format.hpp`'s `parseMinor` cases ported unchanged, plus the
helpers it lost nothing of:

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// fmt::parseMinor turns arbitrary user text into an integer, so it has to survive arbitrary text.
// The out-of-range cases assert rejection rather than a value: converting an out-of-range double to
// an integer is undefined behaviour, which a UBSan build reports as an abort and other builds as a
// wrong number, and only "rejected" fails on both.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <string>

#include "format.hpp"
#include "local_setup.hpp"

using bank::client::fmt::parseMinor;
using bank::client::fmt::parseWhole;

TEST_CASE("parseMinor turns well-formed amounts into minor units", "[bank][app][format]") {
    CHECK(parseMinor("12.34") == 1234);
    CHECK(parseMinor("0") == 0);
    CHECK(parseMinor("  7.5  ") == 750);
    // Half a minor unit rounds away from zero.
    CHECK(parseMinor("0.005") == 1);
    // The decimals come from the selected currency; JPY has none.
    CHECK(parseMinor("1200", 0) == 1200);
}

TEST_CASE("parseMinor rejects text that is not a non-negative amount", "[bank][app][format]") {
    CHECK_FALSE(parseMinor("").has_value());
    CHECK_FALSE(parseMinor("abc").has_value());
    CHECK_FALSE(parseMinor("-1.00").has_value());
    CHECK_FALSE(parseMinor("12abc").has_value());
    CHECK_FALSE(parseMinor("1 2").has_value());
    CHECK_FALSE(parseMinor("0x10").has_value());
    CHECK_FALSE(parseMinor("1e").has_value());
}

// Review Focus 4.
TEST_CASE("parseMinor rejects amounts that do not fit in int64 minor units", "[bank][app][format]") {
    CHECK_FALSE(parseMinor("1e30").has_value());
    CHECK_FALSE(parseMinor("1e300").has_value());
    // Not an absurd magnitude: above ~9.2e16 major units, scaling by 100 overflows.
    CHECK_FALSE(parseMinor("9.3e16").has_value());
    CHECK_FALSE(parseMinor("inf").has_value());
    CHECK_FALSE(parseMinor("nan").has_value());
    // The scale decides the ceiling: what overflows at two decimals fits at none.
    CHECK_FALSE(parseMinor("1e17", 2).has_value());
    CHECK(parseMinor("1e17", 0).has_value());
}

TEST_CASE("parseMinor's ceiling is the int64 range, not an arbitrary cap", "[bank][app][format]") {
    // 9.2e16 major units scale to 9.2e18 minor, just inside 2^63-1.
    CHECK(parseMinor("92000000000000000") == 9200000000000000000LL);
    CHECK_FALSE(parseMinor("920000000000000000").has_value());
}

TEST_CASE("parseMinor rounds a value just below half a minor unit down", "[bank][app][format]") {
    // Scales to 0.49999999999999994, the largest double below 0.5: `+ 0.5` and a truncation give 1,
    // std::llround gives 0.
    CHECK(parseMinor("0.004999999999999999") == 0);
    CHECK(parseMinor("0.0049999999999999994") == 0);
    CHECK_FALSE(parseMinor("1e30").has_value());
}

TEST_CASE("parseWhole accepts whole numbers only", "[bank][app][format]") {
    CHECK(parseWhole("600") == 600);
    CHECK(parseWhole(" 12 ") == 12);
    CHECK(parseWhole("0") == 0);
    CHECK(parseWhole("-3") == -3);
    CHECK_FALSE(parseWhole("").has_value());
    CHECK_FALSE(parseWhole("12abc").has_value());
    CHECK_FALSE(parseWhole("1.5").has_value());
}

TEST_CASE("the display helpers name what the screens show", "[bank][app][format]") {
    using namespace bank::client::fmt;
    CHECK(money(1234, 0) == "12.34 USD");
    CHECK(money(-5, 1) == "-0.05 EUR");
    CHECK(money(1200, 4) == "1200 JPY");
    CHECK(accountKind(1) == "Savings");
    CHECK(accountKind(9) == "Account");
    CHECK(txnKind(2) == "Transfer in");
    CHECK(txnKind(7) == "Loan in");
    CHECK(txnKind(99) == "Entry");
    CHECK(last4("DE00500700100200300400") == "•••• 0400");
    CHECK(last4("12") == "•••• 12");
    CHECK(trimmed("  a b \t") == "a b");
}

TEST_CASE("databaseConnection names the database --db gives", "[bank][app][format]") {
    using bank::client::databaseConnection;
    std::string const fallback = databaseConnection("");
    CHECK(fallback.starts_with("DRIVER=SQLite3;Database="));
    CHECK(fallback.ends_with("morph_bank.db"));
    CHECK(databaseConnection("/tmp/b.db") == "DRIVER=SQLite3;Database=/tmp/b.db");
    CHECK(databaseConnection("DRIVER=SQLite3;Database=x.db;Timeout=5000") ==
          "DRIVER=SQLite3;Database=x.db;Timeout=5000");
}
```

Create `examples/bank/tests/app/test_notices.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <exception>
#include <morph/reactive/signal.hpp>
#include <stdexcept>

#include "notices.hpp"
#include "test_app_context.hpp"

using bank::client::FailureReporter;
using bank::client::Notices;
using bank::client::Tone;
using bank::testing::FakeAppContext;
using std::chrono::milliseconds;

TEST_CASE("Notices: an error shows, then hides after its period", "[bank][app][notices]") {
    FakeAppContext ctx;
    Notices notices{ctx.runtime(), ctx.scheduler()};
    CHECK_FALSE(notices.visible());
    notices.error("boom");
    CHECK(notices.visible());
    CHECK(notices.text() == "boom");
    CHECK(notices.tone() == Tone::Bad);
    ctx.manualScheduler().advance(Notices::kShownFor - milliseconds{1});
    CHECK(notices.visible());
    ctx.manualScheduler().advance(milliseconds{1});
    CHECK_FALSE(notices.visible());
    CHECK(notices.text() == "boom");  // the strip hides; the text stays
}

TEST_CASE("Notices: a second notice restarts the period", "[bank][app][notices]") {
    FakeAppContext ctx;
    Notices notices{ctx.runtime(), ctx.scheduler()};
    notices.error("first");
    ctx.manualScheduler().advance(milliseconds{3000});
    notices.success("second");
    ctx.manualScheduler().advance(milliseconds{3000});
    CHECK(notices.visible());
    CHECK(notices.text() == "second");
    CHECK(notices.tone() == Tone::Good);
    ctx.manualScheduler().advance(milliseconds{200});
    CHECK_FALSE(notices.visible());
}

TEST_CASE("Notices: dismiss hides at once and cancels the pending hide", "[bank][app][notices]") {
    FakeAppContext ctx;
    Notices notices{ctx.runtime(), ctx.scheduler()};
    notices.error("gone");
    notices.dismiss();
    CHECK_FALSE(notices.visible());
    CHECK(ctx.manualScheduler().pendingTimers() == 0);
}

TEST_CASE("FailureReporter: each new failure becomes one error notice", "[bank][app][notices]") {
    FakeAppContext ctx;
    Notices notices{ctx.runtime(), ctx.scheduler()};
    morph::reactive::Signal<std::exception_ptr> failure{ctx.runtime(), nullptr};
    FailureReporter const reporter{ctx.runtime(), notices, {[&failure] { return failure.get(); }}};
    CHECK_FALSE(notices.visible());

    failure.set(std::make_exception_ptr(std::runtime_error{"first"}));
    bank::testing::drain(ctx);
    CHECK(notices.visible());
    CHECK(notices.text() == "first");

    notices.dismiss();
    failure.set(nullptr);
    bank::testing::drain(ctx);
    CHECK_FALSE(notices.visible());

    failure.set(std::make_exception_ptr(std::runtime_error{"second"}));
    bank::testing::drain(ctx);
    CHECK(notices.visible());
    CHECK(notices.text() == "second");
}
```

In `examples/bank/CMakeLists.txt`, place (c) — the test binary, after `catch_discover_tests(bank_tests …)`:

```cmake
        # ── bank_app_tests: the client's controllers and views, headless ─────
        # Links bank_app and no frontend: controllers run over a local bridge on
        # examples/common's FakeAppContext, and views mount on
        # ui::testing::RecordingBackend. The goldens under tests/app/golden are
        # the views' tree dumps.
        add_executable(bank_app_tests
            tests/app/test_format.cpp
            tests/app/test_notices.cpp
        )
        target_include_directories(bank_app_tests PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/tests
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/app)
        target_link_libraries(bank_app_tests PRIVATE bank_app morph_test_main)
        target_compile_definitions(bank_app_tests PRIVATE
            BANK_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/app/golden")
        target_compile_features(bank_app_tests PRIVATE cxx_std_23)
        apply_bigobj(bank_app_tests)
        if(DEFINED AF_SANITIZER)
            apply_sanitizers(bank_app_tests ${AF_SANITIZER})
        endif()
        catch_discover_tests(bank_app_tests
            DISCOVERY_MODE PRE_TEST
            PROPERTIES LABELS "bank")
```

- [ ] **Step 2: Run them to verify they fail**

Run: `cmake -S . -B build/bank && cmake --build build/bank --target bank_app_tests`
Expected: FAIL — `fatal error: 'format.hpp' file not found` (and `'notices.hpp' file not found`).

- [ ] **Step 3: Implement**

Create `examples/bank/app/tone.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

/// @file
/// How a status, an amount or a notice is emphasised, named without a toolkit's colours; a view
/// maps it to a text role.

namespace bank::client {

/// @brief The emphasis a value carries on screen.
enum class Tone : std::uint8_t {
    Neutral,  ///< nothing to signal
    Good,     ///< open, active, paid off, money in, an action that went through
    Warn,     ///< frozen: usable again later
    Bad,      ///< cancelled, money out, a failure
};

}  // namespace bank::client
```

Create `examples/bank/app/choice.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>
#include <vector>

/// @file
/// One entry of a picker, and how a picker's selection follows a list that refreshes.

namespace bank::client {

/// @brief One pickable entry: the id the controller acts on and the label the user reads.
struct Choice {
    std::int64_t id = 0;  ///< the entity id, or the enum value for a fixed list
    std::string label;    ///< display text
    bool operator==(Choice const&) const = default;
};

/// @brief The row the user chose while it is still listed, else the first row, else 0.
///
/// A picker names its first entry until the user picks one, and a refresh that replaces the rows
/// keeps the pick as long as it is still among them.
/// @tparam Row Anything with an `id` member.
/// @param rows The rows as listed now.
/// @param chosen The user's pick; 0 for none.
/// @return The id the screen acts on.
template <typename Row>
[[nodiscard]] std::int64_t resolveChoice(std::vector<Row> const& rows, std::int64_t chosen) {
    for (Row const& row : rows) {
        if (row.id == chosen) {
            return chosen;
        }
    }
    return rows.empty() ? 0 : rows.front().id;
}

}  // namespace bank::client
```

Create `examples/bank/app/format.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <locale>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>

#include "bank/core/money.hpp"
#include "bank/core/types.hpp"

/// @file
/// Display text and amount parsing for the bank's screens. Controllers hand views display-ready
/// strings, so no view does currency arithmetic.

namespace bank::client::fmt {

/// @brief @p text without its leading and trailing ASCII whitespace.
/// @param text Any text.
/// @return A view into @p text.
[[nodiscard]] inline std::string_view trimmed(std::string_view text) noexcept {
    constexpr std::string_view kSpace = " \t\n\r\f\v";
    std::size_t const first = text.find_first_not_of(kSpace);
    if (first == std::string_view::npos) {
        return {};
    }
    return text.substr(first, text.find_last_not_of(kSpace) - first + 1);
}

/// @brief An amount as display text, e.g. `1234` in USD as `"12.34 USD"`.
/// @param minor The amount in minor units.
/// @param currency A `bank::Currency` value.
/// @return The text.
[[nodiscard]] inline std::string money(std::int64_t minor, int currency) {
    return bank::format(bank::Money{.minor = minor, .currency = static_cast<bank::Currency>(currency)});
}

/// @brief An account kind's display name.
/// @param kind A `bank::AccountKind` value.
/// @return The name; `"Account"` for a value the enum does not name.
[[nodiscard]] inline std::string accountKind(int kind) {
    switch (static_cast<bank::AccountKind>(kind)) {
        case bank::AccountKind::Checking:
            return "Checking";
        case bank::AccountKind::Savings:
            return "Savings";
        case bank::AccountKind::Credit:
            return "Credit";
    }
    return "Account";
}

/// @brief A ledger entry's display name.
/// @param kind A `bank::TxnKind` value.
/// @return The name; `"Entry"` for a value the enum does not name.
[[nodiscard]] inline std::string txnKind(int kind) {
    switch (static_cast<bank::TxnKind>(kind)) {
        case bank::TxnKind::Deposit:
            return "Deposit";
        case bank::TxnKind::Withdrawal:
            return "Withdrawal";
        case bank::TxnKind::TransferIn:
            return "Transfer in";
        case bank::TxnKind::TransferOut:
            return "Transfer out";
        case bank::TxnKind::Payment:
            return "Payment";
        case bank::TxnKind::Fee:
            return "Fee";
        case bank::TxnKind::Interest:
            return "Interest";
        case bank::TxnKind::LoanDisbursement:
            return "Loan in";
        case bank::TxnKind::LoanRepayment:
            return "Loan repay";
        case bank::TxnKind::CardPurchase:
            return "Card";
        case bank::TxnKind::Exchange:
            return "Exchange";
    }
    return "Entry";
}

/// @brief An account number masked to its last four characters, e.g. `"•••• 0400"`.
/// @param number The full number.
/// @return The masked text.
[[nodiscard]] inline std::string last4(std::string_view number) {
    std::size_t const keep = number.size() < 4 ? number.size() : 4;
    return "•••• " + std::string{number.substr(number.size() - keep)};
}

/// @brief 2^63, the first `double` that no longer fits in a `std::int64_t`.
///
/// `std::numeric_limits<std::int64_t>::max()` is 2^63-1, which a `double` cannot represent:
/// converting it rounds up to 2^63, so a bound written from it is off by one in the unsafe
/// direction. 2^63 is exact.
inline constexpr double kMinorUnitsBound = 0x1p63;

/// @brief Parses a user-entered amount in major units into minor units.
///
/// Accepts a plain decimal number — digits, a sign, a decimal point, an exponent — read in the
/// classic locale whatever the process locale is (Qt sets it from the environment), after
/// trimming whitespace. Returns `nullopt` for anything that is not a non-negative amount whose
/// scaled value fits in `std::int64_t`. The range check bounds the scaled, unrounded value and runs
/// before the rounding, because rounding a value outside `long long` is undefined; it is written
/// negated so a NaN fails it. Rounding is `std::llround` — to nearest, halves away from zero — not
/// `+ 0.5` and a truncation, which rounds the double just below one half up to one.
/// @param text The user's text.
/// @param decimals The target currency's minor-unit digits.
/// @return The amount in minor units, or `nullopt`.
[[nodiscard]] inline std::optional<std::int64_t> parseMinor(std::string_view text, int decimals = 2) {
    std::string_view const number = trimmed(text);
    if (number.empty() || number.find_first_not_of("0123456789.eE+-") != std::string_view::npos) {
        return std::nullopt;
    }
    std::istringstream stream{std::string{number}};
    stream.imbue(std::locale::classic());
    double major = 0.0;
    stream >> major;
    if (stream.fail() || !stream.eof() || major < 0.0) {
        return std::nullopt;
    }
    double const scaled = major * static_cast<double>(bank::pow10i(decimals));
    if (!(scaled < kMinorUnitsBound)) {
        return std::nullopt;
    }
    return static_cast<std::int64_t>(std::llround(scaled));
}

/// @brief Parses a whole number, such as a rate in basis points or a term in months.
/// @param text The user's text; surrounding whitespace is ignored.
/// @return The number, or `nullopt` when @p text is not exactly one integer.
[[nodiscard]] inline std::optional<int> parseWhole(std::string_view text) {
    std::string_view const number = trimmed(text);
    if (number.empty()) {
        return std::nullopt;
    }
    char const* const last = std::to_address(number.end());
    int value = 0;
    auto const [end, error] = std::from_chars(std::to_address(number.begin()), last, value);
    if (error != std::errc{} || end != last) {
        return std::nullopt;
    }
    return value;
}

}  // namespace bank::client::fmt
```

Create `examples/bank/app/notices.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>
#include <exception>
#include <functional>
#include <memory>
#include <morph/core/bridge.hpp>
#include <morph/core/callback_scope.hpp>
#include <morph/core/completion.hpp>
#include <morph/core/registry.hpp>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/scheduler.hpp>
#include <morph/reactive/signal.hpp>
#include <string>
#include <utility>
#include <vector>

#include "tone.hpp"

/// @file
/// The bank's one notice strip: every failure and every confirmation the screens show, held as
/// reactive state and hidden again after a fixed time on the frontend's scheduler.

namespace bank::client {

/// @brief One message for the strip.
struct Notice {
    std::string text;       ///< what the strip says
    Tone tone = Tone::Bad;  ///< `Bad` for a failure, `Good` for a confirmation
    bool operator==(Notice const&) const = default;
};

/// @brief The shared notice strip. Every controller reports into the same one.
class Notices {
public:
    /// @brief How long a notice stays visible.
    static constexpr std::chrono::milliseconds kShownFor{3200};

    /// @param runtime The runtime. Borrowed: it must outlive this object.
    /// @param scheduler Hides a notice after `kShownFor`. Borrowed: it must outlive this object.
    Notices(morph::reactive::Runtime& runtime, morph::reactive::Scheduler& scheduler);

    /// @brief Shows @p text as a failure.
    void error(std::string text);
    /// @brief Shows @p text as a confirmation.
    void success(std::string text);
    /// @brief Hides the strip now.
    void dismiss();

    /// @brief Whether the strip is showing. Tracked.
    [[nodiscard]] bool visible() const;
    /// @brief The last notice's text; kept after the strip hides. Tracked.
    [[nodiscard]] std::string const& text() const;
    /// @brief The last notice's tone. Tracked.
    [[nodiscard]] Tone tone() const;

private:
    void show(Notice notice);

    morph::reactive::Runtime* _rt;
    morph::reactive::Scheduler* _scheduler;
    morph::reactive::Signal<Notice> _notice;
    morph::reactive::Signal<bool> _visible;
    // Last member, so it is destroyed first: a hide never fires into destroyed signals.
    morph::reactive::TimerHandle _hide;
};

/// @brief Shows each new failure of each source as an error notice.
///
/// One effect per source, so a failure in one does not re-report another's standing error. A
/// source reads a tracked `error()` — a `Query`'s or a `Mutation`'s.
class FailureReporter {
public:
    /// @brief A tracked read of the latest failure, or null.
    using Source = std::function<std::exception_ptr()>;

    /// @param runtime The runtime. Borrowed.
    /// @param notices Receives the failures. Borrowed: it must outlive this object.
    /// @param sources What to watch.
    FailureReporter(morph::reactive::Runtime& runtime, Notices& notices, std::vector<Source> sources);

private:
    std::vector<std::unique_ptr<morph::reactive::Effect>> _effects;
};

/// @brief A `Mutation` runner that also shows @p text as a confirmation when a call succeeds.
/// @tparam Action The action.
/// @tparam Model The handler's model.
/// @tparam Sharing The handler's sharing policy.
/// @param handler Executes the call. Borrowed.
/// @param notices Shows the confirmation. Borrowed.
/// @param scope Gates the confirmation; the owning controller's last member.
/// @param text The confirmation.
/// @return The runner to construct the `Mutation` with.
template <typename Action, typename Model, typename Sharing>
[[nodiscard]] std::function<morph::async::Completion<typename morph::model::ActionTraits<Action>::Result>(Action)>
confirmed(morph::bridge::BridgeHandler<Model, Sharing>& handler, Notices& notices,
          morph::async::CallbackScope const& scope, std::string text) {
    using Result = typename morph::model::ActionTraits<Action>::Result;
    return [&handler, &notices, &scope, text = std::move(text)](Action action) {
        morph::async::Completion<Result> reply = handler.execute(std::move(action));
        reply.then(scope, [&notices, text](Result const& /*result*/) { notices.success(text); });
        return reply;
    };
}

}  // namespace bank::client
```

Create `examples/bank/app/notices.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include "notices.hpp"

#include <morph/reactive/control.hpp>

namespace bank::client {

Notices::Notices(morph::reactive::Runtime& runtime, morph::reactive::Scheduler& scheduler)
    : _rt{&runtime}, _scheduler{&scheduler}, _notice{runtime, Notice{}}, _visible{runtime, false} {}

void Notices::error(std::string text) { show(Notice{.text = std::move(text), .tone = Tone::Bad}); }

void Notices::success(std::string text) { show(Notice{.text = std::move(text), .tone = Tone::Good}); }

void Notices::dismiss() {
    _hide.cancel();
    _visible.set(false);
}

bool Notices::visible() const { return _visible.get(); }

std::string const& Notices::text() const { return _notice.get().text; }

Tone Notices::tone() const { return _notice.get().tone; }

void Notices::show(Notice notice) {
    _hide.cancel();
    _rt->batch([&] {
        _notice.set(std::move(notice));
        _visible.set(true);
    });
    _hide = _scheduler->after(kShownFor, [this] { _visible.set(false); });
}

FailureReporter::FailureReporter(morph::reactive::Runtime& runtime, Notices& notices, std::vector<Source> sources) {
    _effects.reserve(sources.size());
    for (Source& source : sources) {
        _effects.push_back(std::make_unique<morph::reactive::Effect>(runtime, [&notices, source = std::move(source)] {
            if (std::exception_ptr const error = source(); error != nullptr) {
                notices.error(morph::reactive::errorMessage(error));
            }
        }));
    }
}

}  // namespace bank::client
```

Create `examples/bank/app/bank_handlers.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>

#include "bank/models/auth_model.hpp"
#include "bank/models/card_model.hpp"
#include "bank/models/customer_model.hpp"
#include "bank/models/loan_model.hpp"
#include "bank/models/payee_model.hpp"
#include "bank/models/payment_model.hpp"
#include "bank/models/transaction_model.hpp"

/// @file
/// One bridge handler per model the client calls, shared by every controller. Including the
/// model headers here is also what registers those models in any binary that links the app.

namespace bank::client {

/// @brief The client's bridge handlers, all delivering on the frontend's executor.
struct BankHandlers {
    /// @param bridge The connection's bridge. Borrowed: it must outlive this object.
    /// @param callbacks The executor replies land on: the runtime's owner.
    BankHandlers(morph::bridge::Bridge& bridge, morph::exec::IExecutor& callbacks);

    morph::bridge::BridgeHandler<AuthModel> auth;                ///< sign in, register
    morph::bridge::BridgeHandler<CustomerModel> customers;       ///< list and open accounts
    morph::bridge::BridgeHandler<TransactionModel> transactions; ///< deposit, withdraw, transfer, history
    morph::bridge::BridgeHandler<CardModel> cards;               ///< cards
    morph::bridge::BridgeHandler<PayeeModel> payees;             ///< payees
    morph::bridge::BridgeHandler<PaymentModel> payments;         ///< bill payments
    morph::bridge::BridgeHandler<LoanModel> loans;               ///< loans and schedules
};

}  // namespace bank::client
```

Create `examples/bank/app/bank_handlers.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include "bank_handlers.hpp"

namespace bank::client {

BankHandlers::BankHandlers(morph::bridge::Bridge& bridge, morph::exec::IExecutor& callbacks)
    : auth{bridge, &callbacks},
      customers{bridge, &callbacks},
      transactions{bridge, &callbacks},
      cards{bridge, &callbacks},
      payees{bridge, &callbacks},
      payments{bridge, &callbacks},
      loans{bridge, &callbacks} {}

}  // namespace bank::client
```

Create `examples/bank/app/local_setup.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <app/transport.hpp>
#include <string>
#include <string_view>

/// @file
/// How a local (in-process) bank comes up. Native builds migrate a SQLite database; the browser
/// build defines `localSetup()` over its in-memory store instead (wasm/local_setup_wasm.cpp).

namespace bank::client {

/// @brief The ODBC connection string for `--db`.
/// @param database Empty for the default file in the temp directory, a full `DRIVER=…` string as it is,
///        or a path to a SQLite file.
/// @return The connection string.
[[nodiscard]] std::string databaseConnection(std::string_view database);

/// @brief What `examples::connect` runs before a local bridge serves its first call.
/// @return The setup.
[[nodiscard]] morph::examples::LocalSetup localSetup();

}  // namespace bank::client
```

Create `examples/bank/app/local_setup.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include "local_setup.hpp"

#include <filesystem>
#include <string>
#include <string_view>

#include "bank/db/database.hpp"

namespace bank::client {

std::string databaseConnection(std::string_view database) {
    if (database.empty()) {
        return "DRIVER=SQLite3;Database=" + (std::filesystem::temp_directory_path() / "morph_bank.db").string();
    }
    if (database.starts_with("DRIVER=")) {
        return std::string{database};
    }
    return "DRIVER=SQLite3;Database=" + std::string{database};
}

morph::examples::LocalSetup localSetup() {
    return morph::examples::LocalSetup{
        .setupDatabase = [](std::string const& database) { bank::db::setup(databaseConnection(database)); },
    };
}

}  // namespace bank::client
```

Create `examples/bank/app/toolkit_free.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// bank_app is toolkit-free: the same controllers and views run on the terminal and on Qt Quick, and
// only the binary that picks one links either. Qt's imported targets define QT_CORE_LIB (and
// QT_GUI_LIB, QT_QUICK_LIB) on every consumer however indirectly it links them, so a Qt module
// reaching this library's compile line stops the build here instead of letting a controller
// include a Qt header. The browser build links Qt by design and does not compile this file.

#if defined(QT_CORE_LIB) || defined(QT_GUI_LIB) || defined(QT_QUICK_LIB)
#error "bank_app links a Qt module; it must stay toolkit-free"
#endif

namespace bank::client::detail {

/// @brief Gives this translation unit a symbol, so an archiver has nothing to warn about.
extern int const kToolkitFree;
int const kToolkitFree = 1;

}  // namespace bank::client::detail
```

In `examples/bank/CMakeLists.txt`, place (a) — after the `if(NOT TARGET morph::morph) … endif()` block:

```cmake
# morph::ladder_app_common (examples/common/app) is added by the root
# CMakeLists.txt whenever the ladder, the bank example or examples/tui is on;
# bank links it by name, which CMake resolves at generate time. Its PUBLIC
# include root, examples/common, is also where the tests find Part 6's
# header-only testkit/fake_app_context.hpp and testkit/wait.hpp.

# The client's sources: bank_app below compiles them against bank_lib's
# models, and the browser build (wasm/) against the in-memory ones. Absolute,
# so wasm/CMakeLists.txt can name them.
set(BANK_APP_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/app/notices.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/app/bank_handlers.cpp
)
```

and place (b) — after the `# ── CLI driver` block:

```cmake
# ── bank_app: the toolkit-free application ───────────────────────────────────
# Controllers, views and the ui::Application over bank_lib's models. It links
# no toolkit and no frontend: the one binary, bank, links the frontends and
# picks one at run time, and the library's tests need neither. The link check
# below and app/toolkit_free.cpp keep it so. No apply_warnings(), for
# bank_lib's reason: the controllers include the model headers and, through
# them, the ORM's, which are not -Werror clean.
add_library(bank_app STATIC
    ${BANK_APP_SOURCES}
    app/local_setup.cpp
    app/toolkit_free.cpp
)
target_include_directories(bank_app PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/app)
target_link_libraries(bank_app PUBLIC morph::morph bank_lib morph::ladder_app_common)
target_compile_features(bank_app PUBLIC cxx_std_23)
apply_bigobj(bank_app)
if(DEFINED AF_SANITIZER)
    apply_sanitizers(bank_app ${AF_SANITIZER})
endif()
get_target_property(_bank_app_links bank_app LINK_LIBRARIES)
foreach(_bank_app_link IN LISTS _bank_app_links)
    if(_bank_app_link MATCHES "^(Qt6::|morph::qt|morph_qt|morph::tui|morph_tui)")
        message(FATAL_ERROR "bank_app must stay toolkit-free, but it links ${_bank_app_link}")
    endif()
endforeach()
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build/bank --target bank_app_tests && ./build/bank/examples/bank/bank_app_tests "[bank][app]"`
Expected: PASS — 12 test cases.

Mutation checks, each restored afterwards:
1. In `fmt::parseMinor`, delete the `if (!(scaled < kMinorUnitsBound)) { return std::nullopt; }` block. Expected
   FAIL in "parseMinor rejects amounts that do not fit in int64 minor units" at `parseMinor("1e30")`.
2. In `Notices::show`, delete `_hide = _scheduler->after(…);`. Expected FAIL in "Notices: an error shows, then
   hides after its period" at the last `CHECK_FALSE(notices.visible())`.
3. Add `Qt6::Core` to `bank_app`'s `target_link_libraries` and reconfigure. Expected: configure fails with
   `bank_app must stay toolkit-free, but it links Qt6::Core`. Then, instead, add
   `target_compile_definitions(bank_app PRIVATE QT_CORE_LIB)` and build. Expected: `#error "bank_app links a Qt
   module; it must stay toolkit-free"`.

- [ ] **Step 5: Commit**

```bash
git add examples/bank/app examples/bank/tests/app examples/bank/CMakeLists.txt
git commit -m "wip(bank): bank_app with formatting, notices, handlers and the toolkit-free guard

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 2: `AuthController` and the login screen

The old `AppController` + `Login.qml`. Sign-in and registration are `Mutation`s whose runner also adopts the reply;
adoption hangs off the reply itself, not off a change of `lastResult()`, so a reply identical to the previous one
(the same user signing in again) still signs in.

**Files:**
- Create: `examples/bank/app/controllers/auth_controller.hpp`, `examples/bank/app/controllers/auth_controller.cpp`
- Create: `examples/bank/app/views/style.hpp`, `examples/bank/app/views/login_view.hpp`,
  `examples/bank/app/views/login_view.cpp`
- Create: `examples/bank/tests/app/bank_app_test_support.hpp`, `examples/bank/tests/app/recorded.hpp`
- Create: `examples/bank/tests/app/golden/login.txt` (generated in Step 4)
- Modify: `examples/bank/CMakeLists.txt` — append `${CMAKE_CURRENT_SOURCE_DIR}/app/controllers/auth_controller.cpp`
  and `${CMAKE_CURRENT_SOURCE_DIR}/app/views/login_view.cpp` to `set(BANK_APP_SOURCES …)`; append
  `tests/app/test_auth.cpp` to `add_executable(bank_app_tests …)`
- Test: `examples/bank/tests/app/test_auth.cpp`

**Interfaces:**
- Consumes: Task 1's `Notices`, `FailureReporter`, `BankHandlers`, `localSetup`, `FakeAppContext`, `settle`,
  `drain`, `awaitReply`; `reactive::{Store, Mutation, Computed, Signal}` (Part 1); `bridge::Bridge::setDefaultSession(
  session::Context)` (`include/morph/core/bridge.hpp:988`), `session::Context::principal`
  (`include/morph/session/session.hpp:35`); `dto::{LoginRequest, RegisterUser, AuthResult}`
  (`include/bank/dto/auth_dto.hpp`); `ui::{Node, Text, TextInput, Button, Busy, Panel, Column, TextRole,
  TextInputMode, Sizing, SelectOption, Key, Mounted}` and `ui::testing::RecordingBackend{dump, find, prop, edit,
  click}` (Part 2); `examples::{AppEnvironment, Connection, connect, newUuid}` (Part 6);
  `bank::testing::connectionString()` (`examples/bank/tests/bank_test_support.hpp`).
- Produces:
  - `namespace bank::client::login { struct State { Signal<std::string> username, password, displayName; };
    EditUsername, EditPassword, EditDisplayName, ClearForm; using Msg; State init(Runtime&); struct Update; }`.
  - `class AuthController { AuthController(Runtime&, bridge::Bridge&, BankHandlers&, Notices&); editUsername,
    editPassword, editDisplayName, signIn(), createAccount(), signOut(), adopt(dto::AuthResult const&); form();
    signedIn(); principal(); displayName(); handle(); canSignIn(); canCreate(); busy(); }`.
  - `namespace bank::client::views`: `roleOf(Tone)`, `optionsOf(std::vector<Choice> const&)`, `keyOf(std::int64_t)`,
    `accountKeyOf(std::int64_t)`, `idOf(ui::Key const&)` (`views/style.hpp`); `loginView(AuthController&)`.
  - Test support: `bank::testing::{kPassword, localEnvironment(), signUp(FakeAppContext&, AuthController&,
    std::string_view), BankWiring{ctx, connection, handlers, notices, auth, accountReads; runtime(), signUp(prefix),
    openAccount(kind, currency, overdraftMinor = 0), depositInto(id, minor), balanceOf(id)}}`;
    `recorded.hpp`: `RecordingBackend`, `kText`, `kButton`, `kTextInput`, `kSelect`, `kMenu`, `mount`, `node`,
    `button`, `input`, `enabled`, `visible`, `shows`, `checkGolden(name, dump, replacements = {})`.

- [ ] **Step 0: Confirm RecordingBackend's spellings**

```bash
grep -n '"Text"\|"Button"\|"TextInput"\|"Select"\|"Menu"\|"Column"\|"Slot"\|"label"\|"placeholder"\|"enabled"\|"visible"' \
     include/morph/ui/testing/recording_backend.hpp
grep -n 'void chooseIndex\|void choose(\|void edit(\|void click(' include/morph/ui/testing/recording_backend.hpp
```

Expected: every quoted name appears, and the four helpers are declared as Part 2 Task 2 gives them (`chooseIndex`
activates a `Menu` entry). These are Part 2's spellings, which `recorded.hpp` below and every golden use; a
difference means Part 2 has not landed as planned — stop and say so.

- [ ] **Step 1: Write the failing tests**

Create `examples/bank/tests/app/bank_app_test_support.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <app/app_environment.hpp>
#include <app/transport.hpp>
#include <app/uuid.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <memory>
#include <morph/core/bridge.hpp>
#include <morph/reactive/runtime.hpp>
#include <string>
#include <string_view>

#include "bank/dto/account_dto.hpp"
#include "bank/dto/transaction_dto.hpp"
#include "bank/models/account_model.hpp"
#include "bank_handlers.hpp"
#include "bank_test_support.hpp"
#include "controllers/auth_controller.hpp"
#include "local_setup.hpp"
#include "notices.hpp"
#include "test_app_context.hpp"

/// @file
/// A local bank on a test AppContext — the wiring `makeApplication` does, without the frontend — and
/// the calls the controller tests use to arrange data behind a controller's back.

namespace bank::testing {

/// @brief The password every test user registers with.
inline constexpr std::string_view kPassword = "hunter2demo";

/// @brief A local environment over this process's own test database.
[[nodiscard]] inline morph::examples::AppEnvironment localEnvironment() {
    morph::examples::AppEnvironment env;
    env.db = connectionString();
    return env;
}

/// @brief Registers a fresh user through @p auth's form and waits until it is signed in.
/// @param prefix Names the test; a random suffix keeps names unique in one database.
/// @return The principal.
inline std::string signUp(FakeAppContext& ctx, bank::client::AuthController& auth, std::string_view prefix) {
    std::string name = std::string{prefix} + "-" + morph::examples::newUuid().substr(0, 8);
    auth.editUsername(name);
    auth.editPassword(std::string{kPassword});
    auth.editDisplayName("Tester");
    auth.createAccount();
    REQUIRE(settle(ctx, [&auth] { return auth.signedIn(); }));
    return name;
}

/// @brief A connected local bank: the bridge, the handlers, the notice strip and the auth controller
///        every other controller needs.
struct BankWiring {
    FakeAppContext ctx;
    std::unique_ptr<morph::examples::Connection> connection =
        morph::examples::connect(ctx, localEnvironment(), bank::client::localSetup());
    bank::client::BankHandlers handlers{connection->bridge(), connection->callbacks()};
    bank::client::Notices notices{ctx.runtime(), ctx.scheduler()};
    bank::client::AuthController auth{ctx.runtime(), connection->bridge(), handlers, notices};
    morph::bridge::BridgeHandler<bank::AccountModel> accountReads{connection->bridge(), &connection->callbacks()};

    [[nodiscard]] morph::reactive::Runtime& runtime() { return ctx.runtime(); }

    std::string signUp(std::string_view prefix) { return bank::testing::signUp(ctx, auth, prefix); }

    /// @brief Opens an account for the signed-in user, behind every controller's back.
    std::int64_t openAccount(int kind, int currency, std::int64_t overdraftMinor = 0) {
        return awaitReply(ctx, handlers.customers.execute(dto::OpenAccount{
                                   .kind = kind, .currency = currency, .overdraftMinor = overdraftMinor}))
            .id;
    }

    void depositInto(std::int64_t accountId, std::int64_t minor) {
        static_cast<void>(
            awaitReply(ctx, handlers.transactions.execute(dto::Deposit{.accountId = accountId, .amountMinor = minor})));
    }

    /// @brief The balance the model holds, read independently of every controller.
    [[nodiscard]] std::int64_t balanceOf(std::int64_t accountId) {
        return awaitReply(ctx, accountReads.execute(dto::GetAccount{.id = accountId})).balanceMinor;
    }
};

}  // namespace bank::testing
```

Create `examples/bank/tests/app/recorded.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <morph/ui/frontend.hpp>
#include <morph/ui/mount.hpp>
#include <morph/ui/testing/recording_backend.hpp>
#include <morph/ui/view.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "test_app_context.hpp"

/// @file
/// The bank's view tests read RecordingBackend through these helpers only, so its kind and property
/// names are written once.

namespace bank::testing {

using morph::ui::testing::RecordingBackend;

inline constexpr std::string_view kText = "Text";
inline constexpr std::string_view kButton = "Button";
inline constexpr std::string_view kTextInput = "TextInput";
inline constexpr std::string_view kSelect = "Select";
inline constexpr std::string_view kMenu = "Menu";

/// @brief Mounts @p root on @p backend under the context's runtime.
[[nodiscard]] inline std::unique_ptr<morph::ui::Mounted> mount(FakeAppContext& ctx, RecordingBackend& backend,
                                                               morph::ui::Node root) {
    return std::make_unique<morph::ui::Mounted>(ctx.runtime(), backend, std::move(root));
}

/// @brief The widget of @p kind whose @p prop is @p value; fails the test, printing the tree, if none.
[[nodiscard]] inline int node(RecordingBackend const& backend, std::string_view kind, std::string_view prop,
                              std::string_view value) {
    std::optional<int> const found = backend.find(kind, prop, value);
    INFO(backend.dump());
    REQUIRE(found.has_value());
    return found.value_or(-1);
}

[[nodiscard]] inline int button(RecordingBackend const& backend, std::string_view label) {
    return node(backend, kButton, "label", label);
}

[[nodiscard]] inline int input(RecordingBackend const& backend, std::string_view placeholder) {
    return node(backend, kTextInput, "placeholder", placeholder);
}

/// @brief Whether widget @p widgetId is enabled; `enabled` is only set when bound or `false`, so unset means yes.
[[nodiscard]] inline bool enabled(RecordingBackend const& backend, int widgetId) {
    return backend.prop(widgetId, "enabled") != "false";
}

/// @brief Whether widget @p widgetId is visible; `visible` is only set when bound or `false`, so unset means yes.
[[nodiscard]] inline bool visible(RecordingBackend const& backend, int widgetId) {
    return backend.prop(widgetId, "visible") != "false";
}

/// @brief Whether a visible Text says exactly @p text.
[[nodiscard]] inline bool shows(RecordingBackend const& backend, std::string_view text) {
    std::optional<int> const found = backend.find(kText, "text", text);
    return found.has_value() && visible(backend, *found);
}

/// @brief Compares a tree dump with `golden/<name>.txt`.
///
/// With `BANK_UPDATE_GOLDEN` set, writes the file instead and warns: review the diff before
/// committing it, because a golden written from a wrong tree pins the wrong tree.
/// @param replacements Run-specific text (a generated user name) mapped to a stable stand-in first.
inline void checkGolden(std::string_view name, std::string dump,
                        std::vector<std::pair<std::string, std::string>> const& replacements = {}) {
    for (auto const& [from, standIn] : replacements) {
        for (std::size_t pos = dump.find(from); pos != std::string::npos; pos = dump.find(from, pos + standIn.size())) {
            dump.replace(pos, from.size(), standIn);
        }
    }
    std::filesystem::path const path = std::filesystem::path{BANK_GOLDEN_DIR} / (std::string{name} + ".txt");
    if (morph::ui::processEnvironment()("BANK_UPDATE_GOLDEN").has_value()) {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream{path} << dump;
        WARN("rewrote " << path.string());
        return;
    }
    std::ifstream file{path};
    INFO("no golden at " << path.string() << "; generate it with BANK_UPDATE_GOLDEN=1 and review it");
    REQUIRE(file.good());
    std::string const expected{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
    CHECK(dump == expected);
}

}  // namespace bank::testing
```

Create `examples/bank/tests/app/test_auth.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <app/uuid.hpp>
#include <catch2/catch_test_macros.hpp>
#include <string>

#include "bank_app_test_support.hpp"
#include "recorded.hpp"
#include "views/login_view.hpp"

using bank::client::Tone;
using bank::testing::BankWiring;
using bank::testing::kPassword;
using bank::testing::RecordingBackend;
using bank::testing::settle;

TEST_CASE("AuthController: creating an account signs in and installs the session", "[bank][app][auth]") {
    BankWiring wiring;
    std::string const name = wiring.signUp("auth-create");
    CHECK(wiring.auth.principal() == name);
    CHECK(wiring.auth.displayName() == "Tester");
    CHECK(wiring.auth.handle() == "@" + name);
    CHECK(wiring.auth.form().password.peek().empty());
    // Every later call carries the session: OpenAccount refuses a call without a principal.
    CHECK(wiring.openAccount(0, 0) > 0);
}

TEST_CASE("AuthController: a rejected sign-in shows the model's message and stays signed out",
          "[bank][app][auth]") {
    BankWiring wiring;
    wiring.auth.editUsername("nobody-" + morph::examples::newUuid().substr(0, 8));
    wiring.auth.editPassword("whatever");
    wiring.auth.signIn();
    REQUIRE(settle(wiring.ctx, [&wiring] { return wiring.notices.visible(); }));
    CHECK(wiring.notices.text() == "no such user");
    CHECK(wiring.notices.tone() == Tone::Bad);
    CHECK_FALSE(wiring.auth.signedIn());
}

// Review Focus 3.
TEST_CASE("AuthController: signing out and in again as the same user signs in", "[bank][app][auth]") {
    BankWiring wiring;
    std::string const name = wiring.signUp("auth-again");
    for (int round = 0; round < 2; ++round) {
        wiring.auth.signOut();
        CHECK_FALSE(wiring.auth.signedIn());
        CHECK(wiring.auth.principal().empty());
        wiring.auth.editUsername(name);
        wiring.auth.editPassword(std::string{kPassword});
        wiring.auth.signIn();
        // The second round's reply equals the first's; it must still sign in.
        REQUIRE(settle(wiring.ctx, [&wiring] { return wiring.auth.signedIn(); }));
        CHECK(wiring.auth.principal() == name);
    }
}

TEST_CASE("AuthController: the buttons are enabled only for complete input", "[bank][app][auth]") {
    BankWiring wiring;
    CHECK_FALSE(wiring.auth.canSignIn());
    CHECK_FALSE(wiring.auth.canCreate());
    wiring.auth.editUsername("someone");
    CHECK_FALSE(wiring.auth.canSignIn());
    wiring.auth.editPassword("abc");
    CHECK(wiring.auth.canSignIn());
    CHECK_FALSE(wiring.auth.canCreate());  // RegisterUser needs four characters
    wiring.auth.editPassword("abcd");
    CHECK(wiring.auth.canCreate());
}

TEST_CASE("AuthController: busy while a sign-in is in flight", "[bank][app][auth]") {
    BankWiring wiring;
    wiring.auth.editUsername("nobody-" + morph::examples::newUuid().substr(0, 8));
    wiring.auth.editPassword("whatever");
    wiring.auth.signIn();
    CHECK(wiring.auth.busy());
    CHECK_FALSE(wiring.auth.canSignIn());
    REQUIRE(settle(wiring.ctx, [&wiring] { return !wiring.auth.busy(); }));
}

TEST_CASE("Login view: the form drives the controller", "[bank][app][view][auth]") {
    BankWiring wiring;
    RecordingBackend rec;
    auto const mounted = bank::testing::mount(wiring.ctx, rec, bank::client::views::loginView(wiring.auth));
    bank::testing::checkGolden("login", rec.dump());

    int const signIn = bank::testing::button(rec, "Sign in");
    int const create = bank::testing::button(rec, "Create account");
    CHECK_FALSE(bank::testing::enabled(rec, signIn));
    std::string const name = "view-login-" + morph::examples::newUuid().substr(0, 8);
    rec.edit(bank::testing::input(rec, "Username"), name);
    rec.edit(bank::testing::input(rec, "Password"), std::string{kPassword});
    rec.edit(bank::testing::input(rec, "Display name (for new accounts)"), "Viewer");
    bank::testing::drain(wiring.ctx);
    CHECK(wiring.auth.form().username.peek() == name);
    CHECK(bank::testing::enabled(rec, signIn));
    CHECK(bank::testing::enabled(rec, create));

    rec.click(create);
    REQUIRE(settle(wiring.ctx, [&wiring] { return wiring.auth.signedIn(); }));
    CHECK(wiring.auth.displayName() == "Viewer");
    CHECK(wiring.auth.form().username.peek().empty());
}
```

- [ ] **Step 2: Run them to verify they fail**

Add `tests/app/test_auth.cpp` to `bank_app_tests` first. Run: `cmake --build build/bank --target bank_app_tests`
Expected: FAIL — `fatal error: 'controllers/auth_controller.hpp' file not found`.

- [ ] **Step 3: Implement**

Create `examples/bank/app/controllers/auth_controller.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/core/bridge.hpp>
#include <morph/core/callback_scope.hpp>
#include <morph/reactive/control.hpp>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/signal.hpp>
#include <morph/reactive/store.hpp>
#include <string>
#include <variant>

#include "bank/dto/auth_dto.hpp"
#include "bank_handlers.hpp"
#include "notices.hpp"

/// @file
/// Signing in, creating a user and signing out: the login form, the two calls, and the session the
/// bridge then applies to every call.

namespace bank::client {

/// @brief The login form's user intent.
namespace login {

/// @brief What the user has typed.
struct State {
    morph::reactive::Signal<std::string> username;     ///< the user name
    morph::reactive::Signal<std::string> password;     ///< the password
    morph::reactive::Signal<std::string> displayName;  ///< used when creating a user
};

struct EditUsername {
    std::string text;
};
struct EditPassword {
    std::string text;
};
struct EditDisplayName {
    std::string text;
};
/// @brief Empties every field, after a successful sign-in.
struct ClearForm {};

using Msg = std::variant<EditUsername, EditPassword, EditDisplayName, ClearForm>;

/// @brief An empty form.
[[nodiscard]] State init(morph::reactive::Runtime& runtime);

/// @brief Applies one message.
struct Update {
    void operator()(State& state, EditUsername const& msg) const;
    void operator()(State& state, EditPassword const& msg) const;
    void operator()(State& state, EditDisplayName const& msg) const;
    void operator()(State& state, ClearForm const& msg) const;
};

}  // namespace login

/// @brief The session: who is signed in, and the form that signs someone in.
class AuthController {
public:
    /// @param runtime The runtime. Borrowed.
    /// @param bridge Receives the session on sign-in and sign-out. Borrowed.
    /// @param handlers The client's handlers. Borrowed.
    /// @param notices Shows a refusal or failure. Borrowed.
    AuthController(morph::reactive::Runtime& runtime, morph::bridge::Bridge& bridge, BankHandlers& handlers,
                   Notices& notices);

    void editUsername(std::string text);
    void editPassword(std::string text);
    void editDisplayName(std::string text);

    /// @brief Signs in with the form's user name and password; does nothing while `canSignIn()` is false.
    void signIn();
    /// @brief Registers the form's user and signs in; does nothing while `canCreate()` is false.
    void createAccount();
    /// @brief Clears the session.
    void signOut();
    /// @brief Installs @p result's principal as the session, or shows its message when it is a refusal.
    void adopt(dto::AuthResult const& result);

    [[nodiscard]] login::State const& form() const noexcept { return _form.state(); }
    /// @brief Whether someone is signed in. Tracked.
    [[nodiscard]] bool signedIn() const { return _signedIn.get(); }
    [[nodiscard]] std::string const& principal() const { return _principal.get(); }
    [[nodiscard]] std::string const& displayName() const { return _displayName.get(); }
    /// @brief `"@"` and the principal, as the sidebar shows it. Tracked.
    [[nodiscard]] std::string const& handle() const { return _handle.get(); }
    [[nodiscard]] bool canSignIn() const { return _canSignIn.get(); }
    [[nodiscard]] bool canCreate() const { return _canCreate.get(); }
    /// @brief Whether a sign-in or registration is in flight. Tracked.
    [[nodiscard]] bool busy() const { return _busy.get(); }

private:
    morph::reactive::Runtime* _rt;
    morph::bridge::Bridge* _bridge;
    Notices* _notices;
    morph::reactive::Store<login::State, login::Msg> _form;
    morph::reactive::Signal<std::string> _principal;
    morph::reactive::Signal<std::string> _displayName;
    morph::reactive::Mutation<dto::LoginRequest> _login;
    morph::reactive::Mutation<dto::RegisterUser> _register;
    morph::reactive::Computed<bool> _signedIn;
    morph::reactive::Computed<std::string> _handle;
    morph::reactive::Computed<bool> _busy;
    morph::reactive::Computed<bool> _canSignIn;
    morph::reactive::Computed<bool> _canCreate;
    FailureReporter _failures;
    // Last member, so it is destroyed first: a reply still in flight is not adopted.
    morph::async::CallbackScope _adoptions;
};

}  // namespace bank::client
```

Create `examples/bank/app/controllers/auth_controller.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include "controllers/auth_controller.hpp"

#include <morph/core/completion.hpp>
#include <morph/session/session.hpp>
#include <utility>

namespace bank::client {

namespace login {

State init(morph::reactive::Runtime& runtime) {
    return State{.username{runtime, std::string{}},
                 .password{runtime, std::string{}},
                 .displayName{runtime, std::string{}}};
}

void Update::operator()(State& state, EditUsername const& msg) const { state.username.set(msg.text); }
void Update::operator()(State& state, EditPassword const& msg) const { state.password.set(msg.text); }
void Update::operator()(State& state, EditDisplayName const& msg) const { state.displayName.set(msg.text); }

void Update::operator()(State& state, ClearForm const& /*msg*/) const {
    state.username.set(std::string{});
    state.password.set(std::string{});
    state.displayName.set(std::string{});
}

}  // namespace login

AuthController::AuthController(morph::reactive::Runtime& runtime, morph::bridge::Bridge& bridge, BankHandlers& handlers,
                               Notices& notices)
    : _rt{&runtime},
      _bridge{&bridge},
      _notices{&notices},
      _form{runtime, &login::init, login::Update{}},
      _principal{runtime, std::string{}},
      _displayName{runtime, std::string{}},
      // Adoption is attached to each reply, so every successful sign-in installs its session — two
      // identical replies in a row included.
      _login{runtime,
             [this, &handlers](dto::LoginRequest request) {
                 morph::async::Completion<dto::AuthResult> reply = handlers.auth.execute(std::move(request));
                 reply.then(_adoptions, [this](dto::AuthResult const& result) { adopt(result); });
                 return reply;
             }},
      _register{runtime,
                [this, &handlers](dto::RegisterUser request) {
                    morph::async::Completion<dto::AuthResult> reply = handlers.auth.execute(std::move(request));
                    reply.then(_adoptions, [this](dto::AuthResult const& result) { adopt(result); });
                    return reply;
                }},
      _signedIn{runtime, [this] { return !_principal.get().empty(); }},
      _handle{runtime, [this] { return "@" + _principal.get(); }},
      _busy{runtime, [this] { return _login.pending() || _register.pending(); }},
      _canSignIn{runtime,
                 [this] {
                     login::State const& form = _form.state();
                     return !_busy.get() && !form.username.get().empty() && !form.password.get().empty();
                 }},
      _canCreate{runtime,
                 [this] {
                     login::State const& form = _form.state();
                     dto::RegisterUser const draft{.username = form.username.get(),
                                                   .password = form.password.get(),
                                                   .displayName = form.displayName.get()};
                     return !_busy.get() && draft.validate();
                 }},
      _failures{runtime, notices, {[this] { return _login.error(); }, [this] { return _register.error(); }}} {}

void AuthController::editUsername(std::string text) { _form.send(login::EditUsername{std::move(text)}); }
void AuthController::editPassword(std::string text) { _form.send(login::EditPassword{std::move(text)}); }
void AuthController::editDisplayName(std::string text) { _form.send(login::EditDisplayName{std::move(text)}); }

void AuthController::signIn() {
    if (!_canSignIn.peek()) {
        return;
    }
    login::State const& form = _form.state();
    _login.run(dto::LoginRequest{.username = form.username.peek(), .password = form.password.peek()});
}

void AuthController::createAccount() {
    if (!_canCreate.peek()) {
        return;
    }
    login::State const& form = _form.state();
    _register.run(dto::RegisterUser{.username = form.username.peek(),
                                    .password = form.password.peek(),
                                    .displayName = form.displayName.peek()});
}

void AuthController::signOut() {
    _bridge->setDefaultSession(morph::session::Context{});
    _rt->batch([this] {
        _principal.set(std::string{});
        _displayName.set(std::string{});
    });
}

void AuthController::adopt(dto::AuthResult const& result) {
    if (!result.ok) {
        _notices->error(result.message);
        return;
    }
    // The session first: the queries keyed on signedIn() fetch in the flush this batch posts, and
    // their calls must already carry the principal.
    morph::session::Context session;
    session.principal = result.principal;
    _bridge->setDefaultSession(std::move(session));
    _rt->batch([&] {
        _principal.set(result.principal);
        _displayName.set(result.displayName);
        _form.send(login::ClearForm{});
    });
}

}  // namespace bank::client
```

Create `examples/bank/app/views/style.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <morph/ui/view.hpp>
#include <optional>
#include <variant>
#include <vector>

#include "choice.hpp"
#include "tone.hpp"

/// @file
/// The only translation between controllers and views: a controller cannot name a `ui` type, so a
/// tone becomes a text role and a choice a select option here, and nowhere else.

namespace bank::client::views {

/// @brief The text role that shows @p tone. The palette has no warning role, so `Warn` shares the
///        error role.
[[nodiscard]] inline morph::ui::TextRole roleOf(Tone tone) noexcept {
    switch (tone) {
        case Tone::Good:
            return morph::ui::TextRole::Success;
        case Tone::Warn:
        case Tone::Bad:
            return morph::ui::TextRole::Error;
        case Tone::Neutral:
            break;
    }
    return morph::ui::TextRole::Muted;
}

/// @brief @p choices as select options, keyed by id.
[[nodiscard]] inline std::vector<morph::ui::SelectOption> optionsOf(std::vector<Choice> const& choices) {
    std::vector<morph::ui::SelectOption> options;
    options.reserve(choices.size());
    for (Choice const& choice : choices) {
        options.push_back(morph::ui::SelectOption{.key = choice.id, .label = choice.label});
    }
    return options;
}

/// @brief The key of a fixed-list choice, where 0 is a value like any other.
[[nodiscard]] inline std::optional<morph::ui::Key> keyOf(std::int64_t value) { return morph::ui::Key{value}; }

/// @brief The key of an entity choice, where 0 means nothing is selected.
[[nodiscard]] inline std::optional<morph::ui::Key> accountKeyOf(std::int64_t entityId) {
    if (entityId == 0) {
        return std::nullopt;
    }
    return morph::ui::Key{entityId};
}

/// @brief The id a select reported; 0 for a key that is not an id.
[[nodiscard]] inline std::int64_t idOf(morph::ui::Key const& key) noexcept {
    std::int64_t const* const entityId = std::get_if<std::int64_t>(&key);
    return entityId == nullptr ? 0 : *entityId;
}

}  // namespace bank::client::views
```

Create `examples/bank/app/views/login_view.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/ui/view.hpp>

#include "controllers/auth_controller.hpp"

namespace bank::client::views {

/// @brief The sign-in screen.
/// @param auth Owns the form; must outlive the mounted view.
/// @return The view.
[[nodiscard]] morph::ui::Node loginView(AuthController& auth);

}  // namespace bank::client::views
```

Create `examples/bank/app/views/login_view.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include "views/login_view.hpp"

#include <string>
#include <utility>

namespace bank::client::views {

namespace ui = morph::ui;

ui::Node loginView(AuthController& auth) {
    AuthController* const ctl = &auth;
    login::State const* const form = &auth.form();
    return ui::panel({
        .title = "Morph Bank",
        .padding = 1,
        .child = ui::column({
            .children =
                {
                    ui::text({.text = "Sign in to your account", .role = ui::TextRole::Muted}),
                    ui::textInput({.value = [form] { return form->username.get(); },
                                   .onChange = [ctl](std::string text) { ctl->editUsername(std::move(text)); },
                                   .placeholder = "Username"}),
                    ui::textInput({.value = [form] { return form->password.get(); },
                                   .onChange = [ctl](std::string text) { ctl->editPassword(std::move(text)); },
                                   .onSubmit = [ctl](std::string /*text*/) { ctl->signIn(); },
                                   .placeholder = "Password",
                                   .mode = ui::TextInputMode::Password}),
                    ui::textInput({.value = [form] { return form->displayName.get(); },
                                   .onChange = [ctl](std::string text) { ctl->editDisplayName(std::move(text)); },
                                   .placeholder = "Display name (for new accounts)"}),
                    ui::button({.label = "Sign in",
                                .onClick = [ctl] { ctl->signIn(); },
                                .common = {.enabled = [ctl] { return ctl->canSignIn(); }}}),
                    ui::button({.label = "Create account",
                                .onClick = [ctl] { ctl->createAccount(); },
                                .common = {.enabled = [ctl] { return ctl->canCreate(); }}}),
                    ui::busy({.active = [ctl] { return ctl->busy(); }, .label = "Signing in…"}),
                },
            .gap = 1,
        }),
        .common = {.layout = {.width = ui::Sizing::fixed(48)}},
    });
}

}  // namespace bank::client::views
```

Append to `BANK_APP_SOURCES` in `examples/bank/CMakeLists.txt`:

```cmake
    ${CMAKE_CURRENT_SOURCE_DIR}/app/controllers/auth_controller.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/app/views/login_view.cpp
```

- [ ] **Step 4: Generate the golden, then run the tests to verify they pass**

```bash
cmake --build build/bank --target bank_app_tests
BANK_UPDATE_GOLDEN=1 ./build/bank/examples/bank/bank_app_tests "Login view: the form drives the controller"
cat examples/bank/tests/app/golden/login.txt
```

Review the dump before keeping it: one `Panel` titled `Morph Bank` (`layout=fixed(48)/content`) holding a `Column`
with a Text `role=Muted text=Sign in to your account`, three `TextInput`s (placeholders `Username`, `Password` with
`mode=Password`, `Display name (for new accounts)`), Buttons `Sign in` and `Create account` with `enabled=false`, and
a `Busy` with `active=false label=Signing in…` — nothing else. A golden written from a wrong tree pins the wrong
tree.

Run: `./build/bank/examples/bank/bank_app_tests "[bank][app]"`
Expected: PASS — 18 test cases.

Mutation checks, each restored afterwards:
1. In `AuthController::adopt`, delete `_bridge->setDefaultSession(std::move(session));`. Expected FAIL in
   "creating an account signs in and installs the session" — `openAccount` rethrows the model's
   `no session principal`.
2. In `_canSignIn`, delete `&& !form.password.get().empty()`. Expected FAIL in "the buttons are enabled only for
   complete input" at the second `CHECK_FALSE(w.auth.canSignIn())`.
3. In `login_view.cpp`, change the `Create account` button's `onClick` to `[ctl] { ctl->signIn(); }`. Expected FAIL
   in "Login view: the form drives the controller" (times out waiting for `signedIn()`: the user does not exist).

- [ ] **Step 5: Commit**

```bash
git add examples/bank/app examples/bank/tests/app examples/bank/CMakeLists.txt
git commit -m "wip(bank): AuthController and the login screen

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 3: `AccountsController` and the accounts screen

The old `AccountController` + `AccountsPage.qml`: the account list, its total, and the open-account form. The list
is a `Query` keyed on the session, so signing out clears it and signing in fetches it; opening an account
invalidates it.

**Files:**
- Create: `examples/bank/app/controllers/accounts_controller.hpp`,
  `examples/bank/app/controllers/accounts_controller.cpp`
- Create: `examples/bank/app/views/accounts_view.hpp`, `examples/bank/app/views/accounts_view.cpp`
- Create: `examples/bank/tests/app/golden/accounts.txt` (generated in Step 4)
- Modify: `examples/bank/CMakeLists.txt` — append `app/controllers/accounts_controller.cpp` and
  `app/views/accounts_view.cpp` (absolute, as above) to `BANK_APP_SOURCES`; append `tests/app/test_accounts.cpp` to
  `bank_app_tests`
- Test: `examples/bank/tests/app/test_accounts.cpp`

**Interfaces:**
- Consumes: Tasks 1–2 (`Tone`, `Choice`, `fmt::*`, `Notices`, `FailureReporter`, `BankHandlers`,
  `AuthController::signedIn()`, `views::{roleOf, optionsOf, keyOf, idOf}`, `BankWiring`, `recorded.hpp`);
  `reactive::{Query, Mutation, MutationOptions, Store, Computed}` (Part 1); `ui::{forEach, panel, row, column,
  scroll, spacer, select, textInput, button, text, Key, Axis, Sizing}`, `RecordingBackend::{all, choose}` (Part 2);
  `dto::{ListAccounts, AccountList, AccountInfo, OpenAccount, CloseAccount}`, `bank::AccountStatus`.
- Produces: `namespace bank::client::opening { State{kind, currency, overdraft}; ChooseKind, ChooseCurrency,
  EditOverdraft, ClearOverdraft; Msg; init; Update; }`; `struct AccountCard`; `struct AccountsSummary`;
  `class AccountsController { AccountsController(Runtime&, BankHandlers&, AuthController const&, Notices&);
  chooseKind, chooseCurrency, editOverdraft, openAccount(), refresh(); static kindChoices(), currencyChoices();
  form(), cards(), totalText(), openCountText(), loaded(); }`; `views::accountsView(AccountsController&)`.

- [ ] **Step 1: Write the failing tests**

Create `examples/bank/tests/app/test_accounts.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <morph/ui/view.hpp>
#include <vector>

#include "bank/dto/account_dto.hpp"
#include "bank_app_test_support.hpp"
#include "controllers/accounts_controller.hpp"
#include "recorded.hpp"
#include "views/accounts_view.hpp"

using bank::client::AccountCard;
using bank::client::AccountsController;
using bank::client::Tone;
using bank::testing::BankWiring;
using bank::testing::RecordingBackend;
using bank::testing::settle;

TEST_CASE("AccountsController: opening an account lists it with its formatted balance", "[bank][app][accounts]") {
    BankWiring wiring;
    wiring.signUp("acc-open");
    AccountsController accounts{wiring.runtime(), wiring.handlers, wiring.auth, wiring.notices};
    CHECK(accounts.totalText() == "—");
    REQUIRE(settle(wiring.ctx, [&accounts] { return accounts.loaded(); }));
    CHECK(accounts.totalText() == "0");
    CHECK(accounts.openCountText() == "0 open account(s)");

    accounts.chooseKind(0);
    accounts.chooseCurrency(0);
    accounts.editOverdraft("500");
    accounts.openAccount();
    CHECK(accounts.form().overdraft.peek().empty());
    REQUIRE(settle(wiring.ctx, [&accounts] { return accounts.cards().size() == 1; }));
    AccountCard const card = accounts.cards().front();
    CHECK(card.kind == "Checking");
    CHECK(card.number.starts_with("•••• "));
    CHECK(card.balanceText == "0.00 USD");
    CHECK(card.statusText == "Open");
    CHECK(card.statusTone == Tone::Good);
    CHECK(card.hasOverdraft);
    CHECK(card.overdraftText == "Overdraft 500.00 USD");
    CHECK(accounts.totalText() == "0.00 USD");
    CHECK(accounts.openCountText() == "1 open account(s)");
}

TEST_CASE("AccountsController: the total is a sum in one currency and a count across several",
          "[bank][app][accounts]") {
    BankWiring wiring;
    wiring.signUp("acc-total");
    wiring.depositInto(wiring.openAccount(0, 0), 1250);
    AccountsController accounts{wiring.runtime(), wiring.handlers, wiring.auth, wiring.notices};
    REQUIRE(settle(wiring.ctx, [&accounts] { return accounts.cards().size() == 1; }));
    CHECK(accounts.totalText() == "12.50 USD");

    static_cast<void>(wiring.openAccount(1, 1));  // EUR
    accounts.refresh();
    REQUIRE(settle(wiring.ctx, [&accounts] { return accounts.cards().size() == 2; }));
    CHECK(accounts.totalText() == "2");
    CHECK(accounts.openCountText() == "2 open account(s)");
}

TEST_CASE("AccountsController: an overdraft that does not parse opens the account with none",
          "[bank][app][accounts]") {
    BankWiring wiring;
    wiring.signUp("acc-overdraft");
    AccountsController accounts{wiring.runtime(), wiring.handlers, wiring.auth, wiring.notices};
    REQUIRE(settle(wiring.ctx, [&accounts] { return accounts.loaded(); }));
    accounts.editOverdraft("abc");
    accounts.openAccount();
    REQUIRE(settle(wiring.ctx, [&accounts] { return accounts.cards().size() == 1; }));
    CHECK_FALSE(accounts.cards().front().hasOverdraft);
}

TEST_CASE("AccountsController: a closed account is listed but not counted", "[bank][app][accounts]") {
    BankWiring wiring;
    wiring.signUp("acc-closed");
    static_cast<void>(wiring.openAccount(0, 0));
    std::int64_t const closed = wiring.openAccount(1, 0);
    static_cast<void>(
        bank::testing::awaitReply(wiring.ctx, wiring.accountReads.execute(bank::dto::CloseAccount{.id = closed})));
    AccountsController accounts{wiring.runtime(), wiring.handlers, wiring.auth, wiring.notices};
    REQUIRE(settle(wiring.ctx, [&accounts] { return accounts.cards().size() == 2; }));
    CHECK(accounts.cards().back().statusText == "Closed");
    CHECK(accounts.cards().back().statusTone == Tone::Neutral);
    CHECK(accounts.openCountText() == "1 open account(s)");
    CHECK(accounts.totalText() == "0.00 USD");
}

// Review Focus 5.
TEST_CASE("AccountsController: signing out empties the list and the total returns to —", "[bank][app][accounts]") {
    BankWiring wiring;
    wiring.signUp("acc-signout");
    static_cast<void>(wiring.openAccount(0, 0));
    AccountsController accounts{wiring.runtime(), wiring.handlers, wiring.auth, wiring.notices};
    REQUIRE(settle(wiring.ctx, [&accounts] { return accounts.cards().size() == 1; }));
    wiring.auth.signOut();
    bank::testing::drain(wiring.ctx);
    CHECK(accounts.cards().empty());
    CHECK(accounts.totalText() == "—");
    CHECK_FALSE(accounts.loaded());
}

TEST_CASE("Accounts view: the empty screen, then an account opened through it", "[bank][app][view][accounts]") {
    BankWiring wiring;
    wiring.signUp("view-acc");
    AccountsController accounts{wiring.runtime(), wiring.handlers, wiring.auth, wiring.notices};
    REQUIRE(settle(wiring.ctx, [&accounts] { return accounts.loaded(); }));
    RecordingBackend rec;
    auto const mounted = bank::testing::mount(wiring.ctx, rec, bank::client::views::accountsView(accounts));
    bank::testing::checkGolden("accounts", rec.dump());

    std::vector<int> const selects = rec.all(bank::testing::kSelect);
    REQUIRE(selects.size() == 2);
    rec.choose(selects.at(0), morph::ui::Key{std::int64_t{1}});  // Savings
    rec.choose(selects.at(1), morph::ui::Key{std::int64_t{1}});  // EUR
    rec.edit(bank::testing::input(rec, "Overdraft (opt.)"), "25");
    rec.click(bank::testing::button(rec, "Open account"));
    REQUIRE(settle(wiring.ctx, [&accounts] { return accounts.cards().size() == 1; }));
    bank::testing::drain(wiring.ctx);
    CHECK(bank::testing::shows(rec, "Savings"));
    CHECK(bank::testing::shows(rec, "Overdraft 25.00 EUR"));
    CHECK(bank::testing::shows(rec, "0.00 EUR"));
    CHECK(bank::testing::shows(rec, "1 open account(s)"));
}
```

- [ ] **Step 2: Run them to verify they fail**

Run: `cmake --build build/bank --target bank_app_tests`
Expected: FAIL — `fatal error: 'controllers/accounts_controller.hpp' file not found`.

- [ ] **Step 3: Implement**

Create `examples/bank/app/controllers/accounts_controller.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <morph/reactive/control.hpp>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/signal.hpp>
#include <morph/reactive/store.hpp>
#include <string>
#include <variant>
#include <vector>

#include "bank/dto/account_dto.hpp"
#include "bank_handlers.hpp"
#include "choice.hpp"
#include "notices.hpp"
#include "tone.hpp"

/// @file
/// The accounts screen: the signed-in user's accounts, their total, and the open-account form.

namespace bank::client {

class AuthController;

/// @brief The open-account form's user intent.
namespace opening {

struct State {
    morph::reactive::Signal<std::int64_t> kind;      ///< a `bank::AccountKind` value
    morph::reactive::Signal<std::int64_t> currency;  ///< a `bank::Currency` value
    morph::reactive::Signal<std::string> overdraft;  ///< as typed; blank for none
};

struct ChooseKind {
    std::int64_t kind = 0;
};
struct ChooseCurrency {
    std::int64_t currency = 0;
};
struct EditOverdraft {
    std::string text;
};
struct ClearOverdraft {};

using Msg = std::variant<ChooseKind, ChooseCurrency, EditOverdraft, ClearOverdraft>;

/// @brief Checking, USD, no overdraft.
[[nodiscard]] State init(morph::reactive::Runtime& runtime);

struct Update {
    void operator()(State& state, ChooseKind const& msg) const;
    void operator()(State& state, ChooseCurrency const& msg) const;
    void operator()(State& state, EditOverdraft const& msg) const;
    void operator()(State& state, ClearOverdraft const& msg) const;
};

}  // namespace opening

/// @brief One account as the screen shows it.
struct AccountCard {
    std::int64_t id = 0;
    std::string kind;            ///< `Checking`, `Savings`, `Credit`
    std::string number;          ///< masked to its last four
    std::string balanceText;     ///< formatted in the account's currency
    std::string statusText;      ///< `Open` or `Closed`
    Tone statusTone = Tone::Neutral;
    bool hasOverdraft = false;   ///< whether to show `overdraftText`
    std::string overdraftText;   ///< `Overdraft …`
    bool operator==(AccountCard const&) const = default;
};

/// @brief The summary above the list.
struct AccountsSummary {
    std::string totalText;      ///< the sum when the open accounts share a currency, else their count; `—` before a load
    std::string openCountText;  ///< `N open account(s)`
    bool operator==(AccountsSummary const&) const = default;
};

/// @brief The accounts screen's state and commands.
class AccountsController {
public:
    AccountsController(morph::reactive::Runtime& runtime, BankHandlers& handlers, AuthController const& auth,
                       Notices& notices);

    void chooseKind(std::int64_t kind);
    void chooseCurrency(std::int64_t currency);
    void editOverdraft(std::string text);
    /// @brief Opens an account of the chosen kind and currency. A blank or unparseable overdraft opens
    ///        with none, read at two decimals whatever the currency.
    void openAccount();
    /// @brief Fetches the list again.
    void refresh();

    [[nodiscard]] static std::vector<Choice> const& kindChoices();
    [[nodiscard]] static std::vector<Choice> const& currencyChoices();

    [[nodiscard]] opening::State const& form() const noexcept { return _form.state(); }
    [[nodiscard]] std::vector<AccountCard> const& cards() const { return _cards.get(); }
    [[nodiscard]] std::string const& totalText() const { return _summary.get().totalText; }
    [[nodiscard]] std::string const& openCountText() const { return _summary.get().openCountText; }
    /// @brief Whether a list has arrived for the current session. Tracked.
    [[nodiscard]] bool loaded() const { return _list.value().has_value(); }

private:
    morph::reactive::Store<opening::State, opening::Msg> _form;
    morph::reactive::Query<dto::ListAccounts> _list;
    morph::reactive::Mutation<dto::OpenAccount> _open;
    morph::reactive::Computed<std::vector<AccountCard>> _cards;
    morph::reactive::Computed<AccountsSummary> _summary;
    FailureReporter _failures;
};

}  // namespace bank::client
```

Create `examples/bank/app/controllers/accounts_controller.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include "controllers/accounts_controller.hpp"

#include <optional>
#include <string_view>
#include <utility>

#include "bank/core/types.hpp"
#include "controllers/auth_controller.hpp"
#include "format.hpp"

namespace bank::client {

namespace opening {

State init(morph::reactive::Runtime& runtime) {
    return State{.kind{runtime, 0}, .currency{runtime, 0}, .overdraft{runtime, std::string{}}};
}

void Update::operator()(State& state, ChooseKind const& msg) const { state.kind.set(msg.kind); }
void Update::operator()(State& state, ChooseCurrency const& msg) const { state.currency.set(msg.currency); }
void Update::operator()(State& state, EditOverdraft const& msg) const { state.overdraft.set(msg.text); }
void Update::operator()(State& state, ClearOverdraft const& /*msg*/) const { state.overdraft.set(std::string{}); }

}  // namespace opening

namespace {

AccountCard cardOf(dto::AccountInfo const& account) {
    bool const closed = account.status == static_cast<int>(bank::AccountStatus::Closed);
    return AccountCard{
        .id = account.id,
        .kind = fmt::accountKind(account.kind),
        .number = fmt::last4(account.number),
        .balanceText = fmt::money(account.balanceMinor, account.currency),
        .statusText = closed ? "Closed" : "Open",
        .statusTone = closed ? Tone::Neutral : Tone::Good,
        .hasOverdraft = account.overdraftMinor > 0,
        .overdraftText = "Overdraft " + fmt::money(account.overdraftMinor, account.currency),
    };
}

// The currency compared against is the first account's, closed or not; only open accounts count.
AccountsSummary summarize(std::optional<dto::AccountList> const& list) {
    if (!list.has_value()) {
        return AccountsSummary{.totalText = "—", .openCountText = "0 open account(s)"};
    }
    int const currency = list->accounts.empty() ? 0 : list->accounts.front().currency;
    std::int64_t total = 0;
    int openCount = 0;
    bool sameCurrency = true;
    for (dto::AccountInfo const& account : list->accounts) {
        if (account.status == static_cast<int>(bank::AccountStatus::Closed)) {
            continue;
        }
        ++openCount;
        total += account.balanceMinor;
        sameCurrency = sameCurrency && account.currency == currency;
    }
    return AccountsSummary{
        .totalText = sameCurrency && openCount > 0 ? fmt::money(total, currency) : std::to_string(openCount),
        .openCountText = std::to_string(openCount) + " open account(s)",
    };
}

}  // namespace

AccountsController::AccountsController(morph::reactive::Runtime& runtime, BankHandlers& handlers,
                                       AuthController const& auth, Notices& notices)
    : _form{runtime, &opening::init, opening::Update{}},
      _list{runtime, handlers.customers,
            [session = &auth]() -> std::optional<dto::ListAccounts> {
                if (!session->signedIn()) {
                    return std::nullopt;
                }
                return dto::ListAccounts{};
            }},
      _open{runtime, handlers.customers, morph::reactive::MutationOptions{.invalidates = {&_list}}},
      _cards{runtime,
             [this] {
                 std::vector<AccountCard> cards;
                 if (std::optional<dto::AccountList> const& list = _list.value(); list.has_value()) {
                     cards.reserve(list->accounts.size());
                     for (dto::AccountInfo const& account : list->accounts) {
                         cards.push_back(cardOf(account));
                     }
                 }
                 return cards;
             }},
      _summary{runtime, [this] { return summarize(_list.value()); }},
      _failures{runtime, notices, {[this] { return _list.error(); }, [this] { return _open.error(); }}} {}

void AccountsController::chooseKind(std::int64_t kind) { _form.send(opening::ChooseKind{kind}); }
void AccountsController::chooseCurrency(std::int64_t currency) { _form.send(opening::ChooseCurrency{currency}); }
void AccountsController::editOverdraft(std::string text) { _form.send(opening::EditOverdraft{std::move(text)}); }

void AccountsController::openAccount() {
    opening::State const& form = _form.state();
    std::string_view const overdraft = fmt::trimmed(form.overdraft.peek());
    std::int64_t const minor = overdraft.empty() ? 0 : fmt::parseMinor(overdraft).value_or(0);
    _open.run(dto::OpenAccount{.kind = static_cast<int>(form.kind.peek()),
                               .currency = static_cast<int>(form.currency.peek()),
                               .overdraftMinor = minor});
    _form.send(opening::ClearOverdraft{});
}

void AccountsController::refresh() { _list.refetch(); }

std::vector<Choice> const& AccountsController::kindChoices() {
    static std::vector<Choice> const choices{
        {.id = 0, .label = "Checking"}, {.id = 1, .label = "Savings"}, {.id = 2, .label = "Credit"}};
    return choices;
}

std::vector<Choice> const& AccountsController::currencyChoices() {
    static std::vector<Choice> const choices{{.id = 0, .label = "USD"}, {.id = 1, .label = "EUR"},
                                             {.id = 2, .label = "GBP"}, {.id = 3, .label = "CHF"},
                                             {.id = 4, .label = "JPY"}};
    return choices;
}

}  // namespace bank::client
```

Create `examples/bank/app/views/accounts_view.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/ui/view.hpp>

#include "controllers/accounts_controller.hpp"

namespace bank::client::views {

/// @brief The accounts screen: the summary, the open-account form, one panel per account.
/// @param accounts Must outlive the mounted view.
/// @return The view.
[[nodiscard]] morph::ui::Node accountsView(AccountsController& accounts);

}  // namespace bank::client::views
```

Create `examples/bank/app/views/accounts_view.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include "views/accounts_view.hpp"

#include <morph/reactive/signal.hpp>
#include <string>
#include <utility>

#include "views/style.hpp"

namespace bank::client::views {

namespace ui = morph::ui;

ui::Node accountsView(AccountsController& accounts) {
    AccountsController* const ctl = &accounts;
    opening::State const* const form = &accounts.form();

    ui::Node summary = ui::panel({
        .padding = 1,
        .child = ui::column({.children = {
                                 ui::text({.text = [ctl] { return ctl->totalText(); }, .role = ui::TextRole::Heading}),
                                 ui::text({.text = [ctl] { return ctl->openCountText(); }, .role = ui::TextRole::Muted}),
                             }}),
    });

    ui::Node opener = ui::panel({
        .title = "New account",
        .padding = 1,
        .child = ui::row({
            .children =
                {
                    ui::select({.options = optionsOf(AccountsController::kindChoices()),
                                .selected = [form] { return keyOf(form->kind.get()); },
                                .onSelect = [ctl](ui::Key key) { ctl->chooseKind(idOf(key)); }}),
                    ui::select({.options = optionsOf(AccountsController::currencyChoices()),
                                .selected = [form] { return keyOf(form->currency.get()); },
                                .onSelect = [ctl](ui::Key key) { ctl->chooseCurrency(idOf(key)); }}),
                    ui::textInput({.value = [form] { return form->overdraft.get(); },
                                   .onChange = [ctl](std::string text) { ctl->editOverdraft(std::move(text)); },
                                   .placeholder = "Overdraft (opt.)"}),
                    ui::button({.label = "Open account", .onClick = [ctl] { ctl->openAccount(); }}),
                },
            .gap = 1,
        }),
    });

    ui::Node list = ui::forEach<AccountCard>(
        [ctl] { return ctl->cards(); }, [](AccountCard const& card) { return ui::Key{card.id}; },
        [](morph::reactive::Signal<AccountCard> const& card) {
            auto const* const row = &card;
            return ui::panel({
                .padding = 1,
                .child = ui::column({.children = {
                                         ui::row({.children = {
                                                      ui::text({.text = [row] { return row->get().kind; },
                                                                .role = ui::TextRole::Heading}),
                                                      ui::spacer({.common = {.layout = {.width = ui::Sizing::stretch()}}}),
                                                      ui::text({.text = [row] { return row->get().statusText; },
                                                                .role = [row] { return roleOf(row->get().statusTone); }}),
                                                  }}),
                                         ui::text({.text = [row] { return row->get().number; },
                                                   .role = ui::TextRole::Muted}),
                                         ui::text({.text = [row] { return row->get().balanceText; },
                                                   .role = ui::TextRole::Heading}),
                                         ui::text({.text = [row] { return row->get().overdraftText; },
                                                   .role = ui::TextRole::Muted,
                                                   .common = {.visible = [row] { return row->get().hasOverdraft; }}}),
                                     }}),
            });
        },
        ui::Axis::Vertical, 1);

    return ui::column({
        .children = {summary, opener,
                     ui::scroll({.child = list, .common = {.layout = {.height = ui::Sizing::stretch()}}})},
        .gap = 1,
    });
}

}  // namespace bank::client::views
```

- [ ] **Step 4: Generate the golden, then run the tests to verify they pass**

```bash
cmake --build build/bank --target bank_app_tests
BANK_UPDATE_GOLDEN=1 ./build/bank/examples/bank/bank_app_tests "Accounts view: the empty screen, then an account opened through it"
cat examples/bank/tests/app/golden/accounts.txt
```

Review it: a Panel with a Heading Text `0` and a Muted Text `0 open account(s)`; a Panel titled `New account`
holding a Select with options `Checking`, `Savings`, `Credit` and `Checking` selected (`selected=0`), a Select
with options `USD` … `JPY` and `USD` selected, a TextInput `Overdraft (opt.)`, a Button `Open account`; a `Scroll`
holding the list, an empty `Column`. Nothing names an account.

Run: `./build/bank/examples/bank/bank_app_tests "[bank][app]"`
Expected: PASS — 24 test cases.

Mutation checks, each restored afterwards:
1. In `summarize`, delete the `continue;` that skips closed accounts. Expected FAIL in "a closed account is listed
   but not counted" (`"2 open account(s)"`).
2. Make `_list`'s key return `dto::ListAccounts{}` unconditionally. Expected FAIL in "signing out empties the list
   and the total returns to —" (`cards()` keeps one entry).

- [ ] **Step 5: Commit**

```bash
git add examples/bank/app examples/bank/tests/app examples/bank/CMakeLists.txt
git commit -m "wip(bank): AccountsController and the accounts screen

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 4: `TransactionsController` and the move-money screen

The old `TransactionController` + `MoveMoneyPage.qml`, and the controller spec 1 §4b uses as its counter-example.
The old controller fetched history in whichever `.then` remembered to call `reloadHistory()`, so selecting A and
then B let A's late reply overwrite B's history. Here the history is a `Query` keyed on the selection (latest
wins); deposits, withdrawals and transfers are `Mutation`s that invalidate the account list and the history in one
batch; the selection is derived (`resolveChoice`), so the refresh a deposit causes keeps it. This task carries the
two tests that pin both properties: the A/B ordering (Review Focus 1) and the old live-engine QML test's
pick → deposit → still picked → deposit again sequence (Review Focus 2), now without an engine — a view binds the
picker to `selectedAccount()`, so there is no widget state that could drift from the controller's.

**Files:**
- Create: `examples/bank/app/controllers/transactions_controller.hpp`,
  `examples/bank/app/controllers/transactions_controller.cpp`
- Create: `examples/bank/app/views/move_money_view.hpp`, `examples/bank/app/views/move_money_view.cpp`
- Create: `examples/bank/tests/app/golden/move_money.txt` (generated in Step 4)
- Modify: `examples/bank/CMakeLists.txt` — append `app/controllers/transactions_controller.cpp` and
  `app/views/move_money_view.cpp` to `BANK_APP_SOURCES`; append `tests/app/test_transactions.cpp` to `bank_app_tests`
- Test: `examples/bank/tests/app/test_transactions.cpp`

**Interfaces:**
- Consumes: Tasks 1–3; `reactive::Query`'s fetcher constructor `Query(Runtime&, Fetch, Key, QueryOptions = {})`
  (Part 1, the test seam); `async::Completion<T>::makeSettleable(exec::IExecutor*) -> {Completion<T>,
  Completion<T>::Promise}` and `Promise::resolve(T)` (`include/morph/core/completion.hpp:699`); `ui::table<Row>`,
  `ui::TableColumn` (Part 2); `dto::{History, HistoryPage, TxnInfo, Deposit, Withdraw, Transfer, TransferResult}`,
  `bank::{TxnDirection, TxnKind, currencyDecimals}`.
- Produces: `namespace bank::client::movemoney { State{chosenAccount, amount, chosenTarget, transferAmount};
  ChooseAccount, EditAmount, ChooseTarget, EditTransferAmount, ClearAmount, ClearTransferAmount; Msg; init; Update; }`;
  `struct MoveAccount`; `struct HistoryRow`; `using HistoryFetch`; `class TransactionsController {
  TransactionsController(Runtime&, BankHandlers&, AuthController const&, Notices&, HistoryFetch = {});
  chooseAccount, editAmount, chooseTarget, editTransferAmount, deposit(), withdraw(), transfer(), refresh(); form(),
  accountChoices(), selectedAccount(), selectedTarget(), history(), canMove(), canTransfer(), accountsPending(),
  historyPending(); }`; `views::moveMoneyView(TransactionsController&)`.

- [ ] **Step 1: Write the failing tests**

Create `examples/bank/tests/app/test_transactions.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <morph/core/completion.hpp>
#include <morph/core/executor.hpp>
#include <morph/ui/view.hpp>
#include <string>
#include <utility>
#include <vector>

#include "bank/core/types.hpp"
#include "bank/dto/transaction_dto.hpp"
#include "bank_app_test_support.hpp"
#include "controllers/transactions_controller.hpp"
#include "recorded.hpp"
#include "views/move_money_view.hpp"

using bank::client::Choice;
using bank::client::HistoryFetch;
using bank::client::Tone;
using bank::client::TransactionsController;
using bank::testing::BankWiring;
using bank::testing::RecordingBackend;
using bank::testing::settle;

namespace {

// Holds every history request until the test settles it, in whatever order the test chooses.
class HeldHistory {
public:
    explicit HeldHistory(morph::exec::IExecutor& owner) : _owner{&owner} {}

    [[nodiscard]] HistoryFetch fetch() {
        return [this](bank::dto::History const& request) {
            auto [completion, promise] = morph::async::Completion<bank::dto::HistoryPage>::makeSettleable(_owner);
            _calls.push_back(Call{.accountId = request.accountId, .promise = std::move(promise)});
            return std::move(completion);
        };
    }

    [[nodiscard]] std::size_t calls() const noexcept { return _calls.size(); }
    [[nodiscard]] std::int64_t accountOf(std::size_t index) const { return _calls.at(index).accountId; }
    void resolve(std::size_t index, bank::dto::HistoryPage page) { _calls.at(index).promise.resolve(std::move(page)); }

private:
    struct Call {
        std::int64_t accountId = 0;
        morph::async::Completion<bank::dto::HistoryPage>::Promise promise;
    };
    morph::exec::IExecutor* _owner;
    std::vector<Call> _calls;
};

bank::dto::HistoryPage onePage(std::int64_t accountId, bank::TxnKind kind, bank::TxnDirection direction,
                               std::int64_t amountMinor, std::int64_t balanceMinor) {
    return bank::dto::HistoryPage{
        .accountId = accountId,
        .entries = {bank::dto::TxnInfo{.id = accountId * 100,
                                       .accountId = accountId,
                                       .direction = static_cast<int>(direction),
                                       .kind = static_cast<int>(kind),
                                       .amountMinor = amountMinor,
                                       .currency = 0,
                                       .balanceAfterMinor = balanceMinor}},
    };
}

std::string labelOf(TransactionsController const& txns, std::int64_t accountId) {
    for (Choice const& choice : txns.accountChoices()) {
        if (choice.id == accountId) {
            return choice.label;
        }
    }
    return {};
}

}  // namespace

// Review Focus 1.
TEST_CASE("TransactionsController: a late reply for the previously selected account does not replace the "
          "shown history",
          "[bank][app][move-money]") {
    BankWiring wiring;
    wiring.signUp("txn-ab");
    std::int64_t const first = wiring.openAccount(0, 0);
    std::int64_t const second = wiring.openAccount(1, 0);
    HeldHistory held{wiring.ctx.executor()};
    TransactionsController txns{wiring.runtime(), wiring.handlers, wiring.auth, wiring.notices, held.fetch()};

    REQUIRE(settle(wiring.ctx, [&held] { return held.calls() == 1; }));
    REQUIRE(held.accountOf(0) == first);  // the first account, selected until the user picks
    txns.chooseAccount(second);
    REQUIRE(settle(wiring.ctx, [&held] { return held.calls() == 2; }));
    REQUIRE(held.accountOf(1) == second);

    // B answers first; A's reply arrives after it.
    held.resolve(1, onePage(second, bank::TxnKind::Deposit, bank::TxnDirection::Credit, 100, 100));
    held.resolve(0, onePage(first, bank::TxnKind::Withdrawal, bank::TxnDirection::Debit, 999, -999));
    bank::testing::drain(wiring.ctx);

    REQUIRE(txns.history().size() == 1);
    CHECK(txns.history().front().kind == "Deposit");
    CHECK(txns.history().front().amountText == "+1.00 USD");
    CHECK(txns.selectedAccount() == second);
}

// Review Focus 2.
TEST_CASE("TransactionsController: the selection survives the refresh a deposit causes, and the next deposit "
          "lands in the shown account",
          "[bank][app][move-money]") {
    BankWiring wiring;
    wiring.signUp("txn-move");
    std::int64_t const checking = wiring.openAccount(0, 0);
    std::int64_t const savings = wiring.openAccount(1, 0);
    TransactionsController txns{wiring.runtime(), wiring.handlers, wiring.auth, wiring.notices};
    REQUIRE(settle(wiring.ctx, [&txns] { return txns.accountChoices().size() == 2; }));
    CHECK(txns.selectedAccount() == checking);

    txns.chooseAccount(savings);
    REQUIRE(txns.selectedAccount() == savings);
    txns.editAmount("50.00");
    txns.deposit();
    CHECK(txns.form().amount.peek().empty());
    REQUIRE(settle(wiring.ctx, [&txns, savings] { return labelOf(txns, savings).ends_with("50.00 USD"); }));
    CHECK(wiring.balanceOf(savings) == 5000);
    CHECK(txns.selectedAccount() == savings);  // the refresh did not move the selection

    std::int64_t const shown = txns.selectedAccount();
    txns.editAmount("10.00");
    txns.deposit();
    REQUIRE(settle(wiring.ctx, [&txns, savings] { return labelOf(txns, savings).ends_with("60.00 USD"); }));
    CHECK(wiring.balanceOf(shown) == 6000);
    CHECK(wiring.balanceOf(savings) == 6000);
    CHECK(wiring.balanceOf(checking) == 0);
}

TEST_CASE("TransactionsController: a deposit confirms itself in the notice strip", "[bank][app][move-money]") {
    BankWiring wiring;
    wiring.signUp("txn-toast");
    static_cast<void>(wiring.openAccount(0, 0));
    TransactionsController txns{wiring.runtime(), wiring.handlers, wiring.auth, wiring.notices};
    REQUIRE(settle(wiring.ctx, [&txns] { return txns.accountChoices().size() == 1; }));
    txns.editAmount("200.00");
    txns.deposit();
    REQUIRE(settle(wiring.ctx, [&wiring] { return wiring.notices.visible(); }));
    CHECK(wiring.notices.text() == "Transaction posted");
    CHECK(wiring.notices.tone() == Tone::Good);
}

TEST_CASE("TransactionsController: history lists the selected account's entries, newest first",
          "[bank][app][move-money]") {
    BankWiring wiring;
    wiring.signUp("txn-history");
    std::int64_t const account = wiring.openAccount(0, 0);
    TransactionsController txns{wiring.runtime(), wiring.handlers, wiring.auth, wiring.notices};
    REQUIRE(settle(wiring.ctx, [&txns] { return txns.accountChoices().size() == 1; }));
    txns.editAmount("100.00");
    txns.deposit();
    REQUIRE(settle(wiring.ctx, [&txns, account] { return labelOf(txns, account).ends_with("100.00 USD"); }));
    txns.editAmount("30.00");
    txns.withdraw();
    REQUIRE(settle(wiring.ctx, [&txns] { return txns.history().size() == 2 && !txns.historyPending(); }));

    CHECK(txns.history().front().kind == "Withdrawal");
    CHECK(txns.history().front().amountText == "−30.00 USD");
    CHECK(txns.history().front().amountTone == Tone::Bad);
    CHECK(txns.history().front().balanceText == "70.00 USD");
    CHECK(txns.history().back().kind == "Deposit");
    CHECK(txns.history().back().amountText == "+100.00 USD");
    CHECK(txns.history().back().amountTone == Tone::Good);
}

TEST_CASE("TransactionsController: a transfer moves money to the chosen target", "[bank][app][move-money]") {
    BankWiring wiring;
    wiring.signUp("txn-transfer");
    std::int64_t const checking = wiring.openAccount(0, 0);
    std::int64_t const savings = wiring.openAccount(1, 0);
    wiring.depositInto(checking, 10000);
    TransactionsController txns{wiring.runtime(), wiring.handlers, wiring.auth, wiring.notices};
    REQUIRE(settle(wiring.ctx, [&txns] { return txns.accountChoices().size() == 2; }));
    CHECK(txns.selectedTarget() == checking);  // the first account, until the user picks
    txns.chooseTarget(savings);
    txns.editTransferAmount("40.00");
    REQUIRE(txns.canTransfer());
    txns.transfer();
    REQUIRE(settle(wiring.ctx, [&txns, savings] { return labelOf(txns, savings).ends_with("40.00 USD"); }));
    CHECK(wiring.balanceOf(checking) == 6000);
    CHECK(wiring.balanceOf(savings) == 4000);
}

TEST_CASE("TransactionsController: a refused withdrawal shows the model's error", "[bank][app][move-money]") {
    BankWiring wiring;
    wiring.signUp("txn-refused");
    static_cast<void>(wiring.openAccount(0, 0));
    TransactionsController txns{wiring.runtime(), wiring.handlers, wiring.auth, wiring.notices};
    REQUIRE(settle(wiring.ctx, [&txns] { return txns.accountChoices().size() == 1; }));
    txns.editAmount("5.00");
    txns.withdraw();
    REQUIRE(settle(wiring.ctx, [&wiring] { return wiring.notices.visible(); }));
    CHECK(wiring.notices.tone() == Tone::Bad);
    CHECK_FALSE(wiring.notices.text().empty());
    CHECK(wiring.notices.text() != "Transaction posted");
}

TEST_CASE("TransactionsController: amounts use the selected account's currency decimals", "[bank][app][move-money]") {
    BankWiring wiring;
    wiring.signUp("txn-jpy");
    std::int64_t const yen = wiring.openAccount(0, 4);
    TransactionsController txns{wiring.runtime(), wiring.handlers, wiring.auth, wiring.notices};
    REQUIRE(settle(wiring.ctx, [&txns] { return txns.accountChoices().size() == 1; }));
    txns.editAmount("1200");
    txns.deposit();
    REQUIRE(settle(wiring.ctx, [&txns, yen] { return labelOf(txns, yen).ends_with("1200 JPY"); }));
    CHECK(wiring.balanceOf(yen) == 1200);
}

TEST_CASE("TransactionsController: moving money needs a valid amount and an account", "[bank][app][move-money]") {
    BankWiring wiring;
    wiring.signUp("txn-valid");
    TransactionsController txns{wiring.runtime(), wiring.handlers, wiring.auth, wiring.notices};
    REQUIRE(settle(wiring.ctx, [&txns] { return !txns.accountsPending(); }));
    txns.editAmount("5");
    CHECK_FALSE(txns.canMove());  // no account
    static_cast<void>(wiring.openAccount(0, 0));
    txns.refresh();
    REQUIRE(settle(wiring.ctx, [&txns] { return txns.accountChoices().size() == 1; }));
    CHECK(txns.canMove());
    txns.editAmount("abc");
    CHECK_FALSE(txns.canMove());
    CHECK_FALSE(txns.canTransfer());
}

TEST_CASE("Move money view: the picker and the buttons drive the controller", "[bank][app][view][move-money]") {
    BankWiring wiring;
    wiring.signUp("view-move");
    TransactionsController txns{wiring.runtime(), wiring.handlers, wiring.auth, wiring.notices};
    REQUIRE(settle(wiring.ctx, [&txns] { return !txns.accountsPending(); }));
    RecordingBackend rec;
    auto const mounted = bank::testing::mount(wiring.ctx, rec, bank::client::views::moveMoneyView(txns));
    bank::testing::checkGolden("move_money", rec.dump());

    static_cast<void>(wiring.openAccount(0, 0));
    std::int64_t const savings = wiring.openAccount(1, 0);
    txns.refresh();
    REQUIRE(settle(wiring.ctx, [&txns] { return txns.accountChoices().size() == 2; }));
    bank::testing::drain(wiring.ctx);
    std::vector<int> const selects = rec.all(bank::testing::kSelect);
    REQUIRE(selects.size() == 2);  // the account, then the transfer target
    rec.choose(selects.at(0), morph::ui::Key{savings});
    CHECK(txns.selectedAccount() == savings);

    int const deposit = bank::testing::button(rec, "Deposit");
    bank::testing::drain(wiring.ctx);
    CHECK_FALSE(bank::testing::enabled(rec, deposit));
    rec.edit(bank::testing::input(rec, "Amount"), "50.00");
    bank::testing::drain(wiring.ctx);
    CHECK(bank::testing::enabled(rec, deposit));
    rec.click(deposit);
    REQUIRE(settle(wiring.ctx, [&txns] { return txns.history().size() == 1; }));
    bank::testing::drain(wiring.ctx);
    CHECK(wiring.balanceOf(savings) == 5000);
    CHECK(bank::testing::shows(rec, "+50.00 USD"));
}
```

- [ ] **Step 2: Run them to verify they fail**

Run: `cmake --build build/bank --target bank_app_tests`
Expected: FAIL — `fatal error: 'controllers/transactions_controller.hpp' file not found`.

- [ ] **Step 3: Implement**

Create `examples/bank/app/controllers/transactions_controller.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <functional>
#include <morph/core/callback_scope.hpp>
#include <morph/core/completion.hpp>
#include <morph/reactive/control.hpp>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/signal.hpp>
#include <morph/reactive/store.hpp>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "bank/dto/account_dto.hpp"
#include "bank/dto/transaction_dto.hpp"
#include "bank_handlers.hpp"
#include "choice.hpp"
#include "notices.hpp"
#include "tone.hpp"

/// @file
/// The move-money screen: deposit, withdraw and transfer for a selected account, and that account's
/// recent activity.
///
/// The history is a `Query` keyed on the selection, so selecting A and then B supersedes A's
/// request: a late reply for A is dropped instead of overwriting B's history. The selection is the
/// user's pick while it is still listed, else the first open account, so the refresh every
/// successful move causes keeps naming the account the next move lands in.

namespace bank::client {

class AuthController;

/// @brief The move-money form's user intent.
namespace movemoney {

struct State {
    morph::reactive::Signal<std::int64_t> chosenAccount;  ///< the user's pick; 0 for none
    morph::reactive::Signal<std::string> amount;          ///< deposit/withdraw amount, as typed
    morph::reactive::Signal<std::int64_t> chosenTarget;   ///< the user's transfer target; 0 for none
    morph::reactive::Signal<std::string> transferAmount;  ///< transfer amount, as typed
};

struct ChooseAccount {
    std::int64_t id = 0;
};
struct EditAmount {
    std::string text;
};
struct ChooseTarget {
    std::int64_t id = 0;
};
struct EditTransferAmount {
    std::string text;
};
struct ClearAmount {};
struct ClearTransferAmount {};

using Msg = std::variant<ChooseAccount, EditAmount, ChooseTarget, EditTransferAmount, ClearAmount,
                         ClearTransferAmount>;

/// @brief Nothing picked, nothing typed.
[[nodiscard]] State init(morph::reactive::Runtime& runtime);

struct Update {
    void operator()(State& state, ChooseAccount const& msg) const;
    void operator()(State& state, EditAmount const& msg) const;
    void operator()(State& state, ChooseTarget const& msg) const;
    void operator()(State& state, EditTransferAmount const& msg) const;
    void operator()(State& state, ClearAmount const& msg) const;
    void operator()(State& state, ClearTransferAmount const& msg) const;
};

}  // namespace movemoney

/// @brief An open account as the pickers list it.
struct MoveAccount {
    std::int64_t id = 0;
    std::string label;  ///< masked number and balance
    int currency = 0;   ///< what its amounts are parsed in
    bool operator==(MoveAccount const&) const = default;
};

/// @brief One ledger entry as the activity table shows it.
struct HistoryRow {
    std::int64_t id = 0;
    std::string kind;         ///< `Deposit`, `Transfer in`, …
    std::string amountText;   ///< `+` for money in, `−` for money out
    Tone amountTone = Tone::Neutral;
    std::string balanceText;  ///< the balance after the entry
    bool operator==(HistoryRow const&) const = default;
};

/// @brief Fetches one page of an account's history. Production fetches through the bridge; a test
///        passes one whose replies it settles in any order.
using HistoryFetch = std::function<morph::async::Completion<dto::HistoryPage>(dto::History const&)>;

/// @brief The move-money screen's state and commands.
class TransactionsController {
public:
    /// @param history Empty: fetch through `handlers.transactions`.
    TransactionsController(morph::reactive::Runtime& runtime, BankHandlers& handlers, AuthController const& auth,
                           Notices& notices, HistoryFetch history = {});

    void chooseAccount(std::int64_t chosenId);
    void editAmount(std::string text);
    void chooseTarget(std::int64_t chosenId);
    void editTransferAmount(std::string text);
    /// @brief Deposits the amount into the selected account; does nothing while `canMove()` is false.
    void deposit();
    /// @brief Withdraws the amount from the selected account; does nothing while `canMove()` is false.
    void withdraw();
    /// @brief Moves the transfer amount to the target; does nothing while `canTransfer()` is false.
    void transfer();
    /// @brief Fetches the accounts and the history again.
    void refresh();

    [[nodiscard]] movemoney::State const& form() const noexcept { return _form.state(); }
    [[nodiscard]] std::vector<Choice> const& accountChoices() const { return _choices.get(); }
    /// @brief The account the moves act on: the user's pick while listed, else the first. Tracked.
    [[nodiscard]] std::int64_t selectedAccount() const { return _selected.get(); }
    [[nodiscard]] std::int64_t selectedTarget() const { return _target.get(); }
    [[nodiscard]] std::vector<HistoryRow> const& history() const { return _historyRows.get(); }
    [[nodiscard]] bool canMove() const { return _canMove.get(); }
    [[nodiscard]] bool canTransfer() const { return _canTransfer.get(); }
    [[nodiscard]] bool accountsPending() const { return _accounts.pending(); }
    [[nodiscard]] bool historyPending() const { return _history.pending(); }

private:
    morph::reactive::Runtime* _rt;
    morph::reactive::Store<movemoney::State, movemoney::Msg> _form;
    morph::reactive::Query<dto::ListAccounts> _accounts;
    // The history query's key reads these two, and its effect runs in its constructor: they precede it.
    morph::reactive::Computed<std::vector<MoveAccount>> _openAccounts;
    morph::reactive::Computed<std::int64_t> _selected;
    morph::reactive::Query<dto::History> _history;
    morph::reactive::Mutation<dto::Deposit> _deposit;
    morph::reactive::Mutation<dto::Withdraw> _withdraw;
    morph::reactive::Mutation<dto::Transfer> _transfer;
    morph::reactive::Computed<std::vector<Choice>> _choices;
    morph::reactive::Computed<std::int64_t> _target;
    morph::reactive::Computed<int> _currency;
    morph::reactive::Computed<std::vector<HistoryRow>> _historyRows;
    morph::reactive::Computed<std::optional<std::int64_t>> _amountMinor;
    morph::reactive::Computed<std::optional<std::int64_t>> _transferMinor;
    morph::reactive::Computed<bool> _canMove;
    morph::reactive::Computed<bool> _canTransfer;
    FailureReporter _failures;
    // Last member, so it is destroyed first: a confirmation still in flight is dropped.
    morph::async::CallbackScope _confirmations;
};

}  // namespace bank::client
```

Create `examples/bank/app/controllers/transactions_controller.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include "controllers/transactions_controller.hpp"

#include <utility>

#include "bank/core/types.hpp"
#include "controllers/auth_controller.hpp"
#include "format.hpp"

namespace bank::client {

namespace movemoney {

State init(morph::reactive::Runtime& runtime) {
    return State{.chosenAccount{runtime, 0},
                 .amount{runtime, std::string{}},
                 .chosenTarget{runtime, 0},
                 .transferAmount{runtime, std::string{}}};
}

void Update::operator()(State& state, ChooseAccount const& msg) const { state.chosenAccount.set(msg.id); }
void Update::operator()(State& state, EditAmount const& msg) const { state.amount.set(msg.text); }
void Update::operator()(State& state, ChooseTarget const& msg) const { state.chosenTarget.set(msg.id); }
void Update::operator()(State& state, EditTransferAmount const& msg) const { state.transferAmount.set(msg.text); }
void Update::operator()(State& state, ClearAmount const& /*msg*/) const { state.amount.set(std::string{}); }

void Update::operator()(State& state, ClearTransferAmount const& /*msg*/) const {
    state.transferAmount.set(std::string{});
}

}  // namespace movemoney

namespace {

std::vector<MoveAccount> openAccountsOf(std::optional<dto::AccountList> const& list) {
    std::vector<MoveAccount> accounts;
    if (!list.has_value()) {
        return accounts;
    }
    for (dto::AccountInfo const& account : list->accounts) {
        if (account.status == static_cast<int>(bank::AccountStatus::Closed)) {
            continue;
        }
        accounts.push_back(MoveAccount{
            .id = account.id,
            .label = fmt::last4(account.number) + "  ·  " + fmt::money(account.balanceMinor, account.currency),
            .currency = account.currency,
        });
    }
    return accounts;
}

HistoryRow rowOf(dto::TxnInfo const& entry) {
    bool const credit = entry.direction == static_cast<int>(bank::TxnDirection::Credit);
    return HistoryRow{
        .id = entry.id,
        .kind = fmt::txnKind(entry.kind),
        .amountText = (credit ? "+" : "−") + fmt::money(entry.amountMinor, entry.currency),
        .amountTone = credit ? Tone::Good : Tone::Bad,
        .balanceText = fmt::money(entry.balanceAfterMinor, entry.currency),
    };
}

std::optional<std::int64_t> amountIn(std::string const& text, int currency) {
    return fmt::parseMinor(text, bank::currencyDecimals(static_cast<bank::Currency>(currency)));
}

}  // namespace

TransactionsController::TransactionsController(morph::reactive::Runtime& runtime, BankHandlers& handlers,
                                               AuthController const& auth, Notices& notices, HistoryFetch history)
    : _rt{&runtime},
      _form{runtime, &movemoney::init, movemoney::Update{}},
      _accounts{runtime, handlers.customers,
                [session = &auth]() -> std::optional<dto::ListAccounts> {
                    if (!session->signedIn()) {
                        return std::nullopt;
                    }
                    return dto::ListAccounts{};
                }},
      _openAccounts{runtime, [this] { return openAccountsOf(_accounts.value()); }},
      _selected{runtime, [this] { return resolveChoice(_openAccounts.get(), _form.state().chosenAccount.get()); }},
      _history{runtime,
               history ? std::move(history)
                       : HistoryFetch{[&handlers](dto::History const& request) {
                             return handlers.transactions.execute(request);
                         }},
               [this]() -> std::optional<dto::History> {
                   std::int64_t const account = _selected.get();
                   if (account == 0) {
                       return std::nullopt;
                   }
                   return dto::History{.accountId = account, .limit = 50};
               }},
      _deposit{runtime, confirmed<dto::Deposit>(handlers.transactions, notices, _confirmations, "Transaction posted"),
               morph::reactive::MutationOptions{.invalidates = {&_accounts, &_history}}},
      _withdraw{runtime, confirmed<dto::Withdraw>(handlers.transactions, notices, _confirmations, "Transaction posted"),
                morph::reactive::MutationOptions{.invalidates = {&_accounts, &_history}}},
      _transfer{runtime, confirmed<dto::Transfer>(handlers.transactions, notices, _confirmations, "Transaction posted"),
                morph::reactive::MutationOptions{.invalidates = {&_accounts, &_history}}},
      _choices{runtime,
               [this] {
                   std::vector<Choice> choices;
                   for (MoveAccount const& account : _openAccounts.get()) {
                       choices.push_back(Choice{.id = account.id, .label = account.label});
                   }
                   return choices;
               }},
      _target{runtime, [this] { return resolveChoice(_openAccounts.get(), _form.state().chosenTarget.get()); }},
      _currency{runtime,
                [this] {
                    std::int64_t const selected = _selected.get();
                    for (MoveAccount const& account : _openAccounts.get()) {
                        if (account.id == selected) {
                            return account.currency;
                        }
                    }
                    return 0;
                }},
      _historyRows{runtime,
                   [this] {
                       std::vector<HistoryRow> rows;
                       if (std::optional<dto::HistoryPage> const& page = _history.value(); page.has_value()) {
                           rows.reserve(page->entries.size());
                           for (dto::TxnInfo const& entry : page->entries) {
                               rows.push_back(rowOf(entry));
                           }
                       }
                       return rows;
                   }},
      _amountMinor{runtime, [this] { return amountIn(_form.state().amount.get(), _currency.get()); }},
      _transferMinor{runtime, [this] { return amountIn(_form.state().transferAmount.get(), _currency.get()); }},
      _canMove{runtime, [this] { return _amountMinor.get().has_value() && _selected.get() != 0; }},
      _canTransfer{runtime,
                   [this] {
                       return _transferMinor.get().has_value() && _selected.get() != 0 && _target.get() != 0;
                   }},
      _failures{runtime, notices,
                {[this] { return _accounts.error(); }, [this] { return _history.error(); },
                 [this] { return _deposit.error(); }, [this] { return _withdraw.error(); },
                 [this] { return _transfer.error(); }}} {}

void TransactionsController::chooseAccount(std::int64_t chosenId) { _form.send(movemoney::ChooseAccount{chosenId}); }
void TransactionsController::editAmount(std::string text) { _form.send(movemoney::EditAmount{std::move(text)}); }
void TransactionsController::chooseTarget(std::int64_t chosenId) { _form.send(movemoney::ChooseTarget{chosenId}); }

void TransactionsController::editTransferAmount(std::string text) {
    _form.send(movemoney::EditTransferAmount{std::move(text)});
}

void TransactionsController::deposit() {
    if (!_canMove.peek()) {
        return;
    }
    _deposit.run(dto::Deposit{.accountId = _selected.peek(), .amountMinor = _amountMinor.peek().value_or(0)});
    _form.send(movemoney::ClearAmount{});
}

void TransactionsController::withdraw() {
    if (!_canMove.peek()) {
        return;
    }
    _withdraw.run(dto::Withdraw{.accountId = _selected.peek(), .amountMinor = _amountMinor.peek().value_or(0)});
    _form.send(movemoney::ClearAmount{});
}

void TransactionsController::transfer() {
    if (!_canTransfer.peek()) {
        return;
    }
    _transfer.run(dto::Transfer{.fromAccountId = _selected.peek(),
                                .toAccountId = _target.peek(),
                                .amountMinor = _transferMinor.peek().value_or(0)});
    _form.send(movemoney::ClearTransferAmount{});
}

void TransactionsController::refresh() {
    _rt->batch([this] {
        _accounts.refetch();
        _history.refetch();
    });
}

}  // namespace bank::client
```

Create `examples/bank/app/views/move_money_view.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/ui/view.hpp>

#include "controllers/transactions_controller.hpp"

namespace bank::client::views {

/// @brief The move-money screen: the account picker, deposit/withdraw, transfer, recent activity.
/// @param txns Must outlive the mounted view.
/// @return The view.
[[nodiscard]] morph::ui::Node moveMoneyView(TransactionsController& txns);

}  // namespace bank::client::views
```

Create `examples/bank/app/views/move_money_view.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include "views/move_money_view.hpp"

#include <morph/reactive/signal.hpp>
#include <string>
#include <utility>
#include <vector>

#include "views/style.hpp"

namespace bank::client::views {

namespace ui = morph::ui;

ui::Node moveMoneyView(TransactionsController& txns) {
    TransactionsController* const ctl = &txns;
    movemoney::State const* const form = &txns.form();
    auto const accountOptions = [ctl] { return optionsOf(ctl->accountChoices()); };
    ui::Common const stretch{.layout = {.width = ui::Sizing::stretch()}};

    ui::Node pickRow = ui::row({
        .children = {ui::text({.text = "Account", .role = ui::TextRole::Muted}),
                     ui::select({.options = accountOptions,
                                 .selected = [ctl] { return accountKeyOf(ctl->selectedAccount()); },
                                 .onSelect = [ctl](ui::Key key) { ctl->chooseAccount(idOf(key)); },
                                 .common = stretch})},
        .gap = 1,
    });

    ui::Node amountRow = ui::row({
        .children = {ui::textInput({.value = [form] { return form->amount.get(); },
                                    .onChange = [ctl](std::string text) { ctl->editAmount(std::move(text)); },
                                    .placeholder = "Amount",
                                    .common = stretch}),
                     ui::button({.label = "Deposit",
                                 .onClick = [ctl] { ctl->deposit(); },
                                 .common = {.enabled = [ctl] { return ctl->canMove(); }}}),
                     ui::button({.label = "Withdraw",
                                 .onClick = [ctl] { ctl->withdraw(); },
                                 .common = {.enabled = [ctl] { return ctl->canMove(); }}})},
        .gap = 1,
    });

    ui::Node transferRow = ui::row({
        .children = {ui::button({.label = "Transfer to",
                                 .onClick = [ctl] { ctl->transfer(); },
                                 .common = {.enabled = [ctl] { return ctl->canTransfer(); }}}),
                     ui::select({.options = accountOptions,
                                 .selected = [ctl] { return accountKeyOf(ctl->selectedTarget()); },
                                 .onSelect = [ctl](ui::Key key) { ctl->chooseTarget(idOf(key)); },
                                 .common = stretch}),
                     ui::textInput({.value = [form] { return form->transferAmount.get(); },
                                    .onChange = [ctl](std::string text) { ctl->editTransferAmount(std::move(text)); },
                                    .placeholder = "Transfer amount",
                                    .common = stretch})},
        .gap = 1,
    });

    ui::Node activity = ui::table<HistoryRow>(
        {ui::TableColumn{.label = "TYPE", .width = ui::Sizing::stretch()},
         ui::TableColumn{.label = "AMOUNT", .width = ui::Sizing::fixed(16)},
         ui::TableColumn{.label = "BALANCE", .width = ui::Sizing::fixed(16)}},
        [ctl] { return ctl->history(); }, [](HistoryRow const& row) { return ui::Key{row.id}; },
        [](morph::reactive::Signal<HistoryRow> const& entry) {
            auto const* const row = &entry;
            return std::vector<ui::Node>{
                ui::text({.text = [row] { return row->get().kind; }}),
                ui::text({.text = [row] { return row->get().amountText; },
                          .role = [row] { return roleOf(row->get().amountTone); }}),
                ui::text({.text = [row] { return row->get().balanceText; }}),
            };
        });

    return ui::column({
        .children = {ui::panel({.title = "Move money",
                                .padding = 1,
                                .child = ui::column({.children = {pickRow, amountRow, transferRow}, .gap = 1})}),
                     ui::panel({.title = "Recent activity",
                                .padding = 1,
                                .child = activity,
                                .common = {.layout = {.height = ui::Sizing::stretch()}}})},
        .gap = 1,
    });
}

}  // namespace bank::client::views
```

- [ ] **Step 4: Generate the golden, then run the tests to verify they pass**

```bash
cmake --build build/bank --target bank_app_tests
BANK_UPDATE_GOLDEN=1 ./build/bank/examples/bank/bank_app_tests "Move money view: the picker and the buttons drive the controller"
cat examples/bank/tests/app/golden/move_money.txt
```

Review it: a Panel `Move money` with three `Row`s — a Muted Text `Account` and a Select with `options=[]
selected=none`; a TextInput `Amount` and Buttons `Deposit`, `Withdraw` with `enabled=false`; a Button `Transfer to`
(`enabled=false`), a Select with `options=[] selected=none`, a TextInput `Transfer amount` — and a Panel `Recent
activity` with a `Table` of `columns=[TYPE:stretch(1),AMOUNT:fixed(16),BALANCE:fixed(16)]` and no `Row`s.

Run: `./build/bank/examples/bank/bank_app_tests "[bank][app]"`
Expected: PASS — 33 test cases.

Mutation checks, each restored afterwards:
1. In `include/morph/reactive/control.hpp`, `Query::issue`, delete `_inflight.reset();` — the supersede this
   controller's history relies on. Expected FAIL in "a late reply for the previously selected account does not
   replace the shown history" (`kind == "Withdrawal"`).
2. In `_selected`, pass `0` instead of `_form.state().chosenAccount.get()` to `resolveChoice`. Expected FAIL in
   "the selection survives the refresh a deposit causes, …" at `REQUIRE(txns.selectedAccount() == savings)`.
3. Remove `&_history` from `_withdraw`'s `invalidates`. Expected FAIL in "history lists the selected account's
   entries, newest first" (times out with one entry).

- [ ] **Step 5: Commit**

```bash
git add examples/bank/app examples/bank/tests/app examples/bank/CMakeLists.txt
git commit -m "wip(bank): TransactionsController, a history keyed on the selection, and the move-money screen

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 5: `CardsController` and the cards screen

The old `CardController` + `CardsPage.qml`: issue a card on an account, then freeze, unfreeze or cancel it. The
freeze/unfreeze choice — a conditional the QML made on `modelData.active` — moves into `toggleFreeze`.

**Files:**
- Create: `examples/bank/app/account_choices.hpp`
- Create: `examples/bank/app/controllers/cards_controller.hpp`, `examples/bank/app/controllers/cards_controller.cpp`
- Create: `examples/bank/app/views/cards_view.hpp`, `examples/bank/app/views/cards_view.cpp`
- Create: `examples/bank/tests/app/golden/cards.txt` (generated in Step 4)
- Modify: `examples/bank/CMakeLists.txt` — append `app/controllers/cards_controller.cpp` and `app/views/cards_view.cpp`
  to `BANK_APP_SOURCES`; append `tests/app/test_cards.cpp` to `bank_app_tests`
- Test: `examples/bank/tests/app/test_cards.cpp`

**Interfaces:**
- Consumes: Tasks 1–4; `dto::{ListCards, CardList, CardInfo, IssueCard, FreezeCard, UnfreezeCard, CancelCard,
  CommandResult}` (`include/bank/dto/card_dto.hpp`), `bank::{CardKind, CardStatus}`.
- Produces: `openAccountChoices(std::optional<dto::AccountList> const&) -> std::vector<Choice>`
  (`account_choices.hpp`, used again by Tasks 6–7); `namespace bank::client::issuing { State{account, kind, limit};
  ChooseAccount, ChooseKind, EditLimit, ClearLimit; Msg; init; Update; }`; `struct CardRow`; `class CardsController {
  CardsController(Runtime&, BankHandlers&, AuthController const&, Notices&); chooseAccount, chooseKind, editLimit,
  issue(), toggleFreeze(id), cancel(id), refresh(); static kindChoices(); form(), accountChoices(),
  selectedAccount(), cards(), canIssue(), loaded(); }`; `views::cardsView(CardsController&)`.

- [ ] **Step 1: Write the failing tests**

Create `examples/bank/tests/app/test_cards.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <cstdint>

#include "bank_app_test_support.hpp"
#include "controllers/cards_controller.hpp"
#include "recorded.hpp"
#include "views/cards_view.hpp"

using bank::client::CardRow;
using bank::client::CardsController;
using bank::client::Tone;
using bank::testing::BankWiring;
using bank::testing::RecordingBackend;
using bank::testing::settle;

TEST_CASE("CardsController: an issued card is listed with its limit and can be frozen, unfrozen and cancelled",
          "[bank][app][cards]") {
    BankWiring wiring;
    wiring.signUp("cards-life");
    std::int64_t const account = wiring.openAccount(0, 0);
    CardsController cards{wiring.runtime(), wiring.handlers, wiring.auth, wiring.notices};
    REQUIRE(settle(wiring.ctx, [&cards] { return cards.accountChoices().size() == 1 && cards.loaded(); }));
    CHECK(cards.selectedAccount() == account);

    cards.chooseKind(0);
    cards.editLimit("1000");
    cards.issue();
    CHECK(cards.form().limit.peek().empty());
    REQUIRE(settle(wiring.ctx, [&cards] { return cards.cards().size() == 1; }));
    CardRow const issued = cards.cards().front();
    CHECK(issued.title.starts_with("Debit card  ••••"));
    CHECK(issued.limitText == "Daily limit 1000.00 USD");
    CHECK(issued.statusText == "Active");
    CHECK(issued.statusTone == Tone::Good);
    CHECK(issued.toggleLabel == "Freeze");
    CHECK(issued.active);
    CHECK_FALSE(issued.cancelled);

    cards.toggleFreeze(issued.id);
    REQUIRE(settle(wiring.ctx, [&cards] { return cards.cards().front().statusText == "Frozen"; }));
    CHECK(cards.cards().front().statusTone == Tone::Warn);
    CHECK(cards.cards().front().toggleLabel == "Unfreeze");

    cards.toggleFreeze(issued.id);
    REQUIRE(settle(wiring.ctx, [&cards] { return cards.cards().front().statusText == "Active"; }));

    cards.cancel(issued.id);
    REQUIRE(settle(wiring.ctx, [&cards] { return cards.cards().front().cancelled; }));
    CHECK(cards.cards().front().statusText == "Cancelled");
    CHECK(cards.cards().front().statusTone == Tone::Bad);
}

TEST_CASE("CardsController: issuing needs an account", "[bank][app][cards]") {
    BankWiring wiring;
    wiring.signUp("cards-none");
    CardsController cards{wiring.runtime(), wiring.handlers, wiring.auth, wiring.notices};
    REQUIRE(settle(wiring.ctx, [&cards] { return cards.loaded(); }));
    CHECK_FALSE(cards.canIssue());
    CHECK(cards.selectedAccount() == 0);
}

TEST_CASE("CardsController: a blank limit issues a card with none", "[bank][app][cards]") {
    BankWiring wiring;
    wiring.signUp("cards-blank");
    static_cast<void>(wiring.openAccount(0, 0));
    CardsController cards{wiring.runtime(), wiring.handlers, wiring.auth, wiring.notices};
    REQUIRE(settle(wiring.ctx, [&cards] { return cards.canIssue(); }));
    cards.chooseKind(1);
    cards.issue();
    REQUIRE(settle(wiring.ctx, [&cards] { return cards.cards().size() == 1; }));
    CHECK(cards.cards().front().title.starts_with("Credit card  ••••"));
    CHECK(cards.cards().front().limitText == "Daily limit 0.00 USD");
}

TEST_CASE("Cards view: issuing, freezing and cancelling through the screen", "[bank][app][view][cards]") {
    BankWiring wiring;
    wiring.signUp("view-cards");
    CardsController cards{wiring.runtime(), wiring.handlers, wiring.auth, wiring.notices};
    REQUIRE(settle(wiring.ctx, [&cards] { return cards.loaded(); }));
    RecordingBackend rec;
    auto const mounted = bank::testing::mount(wiring.ctx, rec, bank::client::views::cardsView(cards));
    bank::testing::checkGolden("cards", rec.dump());

    static_cast<void>(wiring.openAccount(0, 0));
    cards.refresh();
    REQUIRE(settle(wiring.ctx, [&cards] { return cards.canIssue(); }));
    rec.edit(bank::testing::input(rec, "Daily limit (opt.)"), "250");
    rec.click(bank::testing::button(rec, "Issue"));
    REQUIRE(settle(wiring.ctx, [&cards] { return cards.cards().size() == 1; }));
    bank::testing::drain(wiring.ctx);
    CHECK(bank::testing::shows(rec, "Daily limit 250.00 USD"));

    rec.click(bank::testing::button(rec, "Freeze"));
    REQUIRE(settle(wiring.ctx, [&cards] { return cards.cards().front().statusText == "Frozen"; }));
    bank::testing::drain(wiring.ctx);
    CHECK(bank::testing::shows(rec, "Frozen"));

    rec.click(bank::testing::button(rec, "Cancel"));
    REQUIRE(settle(wiring.ctx, [&cards] { return cards.cards().front().cancelled; }));
    bank::testing::drain(wiring.ctx);
    CHECK_FALSE(bank::testing::visible(rec, bank::testing::button(rec, "Unfreeze")));
    CHECK_FALSE(bank::testing::visible(rec, bank::testing::button(rec, "Cancel")));
}
```

- [ ] **Step 2: Run them to verify they fail**

Run: `cmake --build build/bank --target bank_app_tests`
Expected: FAIL — `fatal error: 'controllers/cards_controller.hpp' file not found`.

- [ ] **Step 3: Implement**

Create `examples/bank/app/account_choices.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <optional>
#include <vector>

#include "bank/core/types.hpp"
#include "bank/dto/account_dto.hpp"
#include "choice.hpp"
#include "format.hpp"

/// @file
/// The signed-in user's open accounts as picker choices, labelled by masked number: what the cards,
/// payees and loans screens pick an account from.

namespace bank::client {

/// @brief The open accounts in @p list, in list order; empty before a list has arrived.
[[nodiscard]] inline std::vector<Choice> openAccountChoices(std::optional<dto::AccountList> const& list) {
    std::vector<Choice> choices;
    if (!list.has_value()) {
        return choices;
    }
    for (dto::AccountInfo const& account : list->accounts) {
        if (account.status == static_cast<int>(bank::AccountStatus::Closed)) {
            continue;
        }
        choices.push_back(Choice{.id = account.id, .label = fmt::last4(account.number)});
    }
    return choices;
}

}  // namespace bank::client
```

Create `examples/bank/app/controllers/cards_controller.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <morph/reactive/control.hpp>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/signal.hpp>
#include <morph/reactive/store.hpp>
#include <string>
#include <variant>
#include <vector>

#include "bank/dto/account_dto.hpp"
#include "bank/dto/card_dto.hpp"
#include "bank_handlers.hpp"
#include "choice.hpp"
#include "notices.hpp"
#include "tone.hpp"

/// @file
/// The cards screen: issue a card on an account, and freeze, unfreeze or cancel one.

namespace bank::client {

class AuthController;

/// @brief The issue-card form's user intent.
namespace issuing {

struct State {
    morph::reactive::Signal<std::int64_t> account;  ///< the user's pick; 0 for none
    morph::reactive::Signal<std::int64_t> kind;     ///< a `bank::CardKind` value
    morph::reactive::Signal<std::string> limit;     ///< the daily limit as typed; blank for none
};

struct ChooseAccount {
    std::int64_t id = 0;
};
struct ChooseKind {
    std::int64_t kind = 0;
};
struct EditLimit {
    std::string text;
};
struct ClearLimit {};

using Msg = std::variant<ChooseAccount, ChooseKind, EditLimit, ClearLimit>;

[[nodiscard]] State init(morph::reactive::Runtime& runtime);

struct Update {
    void operator()(State& state, ChooseAccount const& msg) const;
    void operator()(State& state, ChooseKind const& msg) const;
    void operator()(State& state, EditLimit const& msg) const;
    void operator()(State& state, ClearLimit const& msg) const;
};

}  // namespace issuing

/// @brief One card as the screen shows it.
struct CardRow {
    std::int64_t id = 0;
    std::string title;        ///< `Debit card  ••••1234`
    std::string limitText;    ///< `Daily limit …`
    std::string statusText;   ///< `Active`, `Frozen`, `Cancelled`
    Tone statusTone = Tone::Neutral;
    bool active = false;
    bool cancelled = false;   ///< a cancelled card offers no actions
    std::string toggleLabel;  ///< `Freeze` for an active card, else `Unfreeze`
    bool operator==(CardRow const&) const = default;
};

/// @brief The cards screen's state and commands.
class CardsController {
public:
    CardsController(morph::reactive::Runtime& runtime, BankHandlers& handlers, AuthController const& auth,
                    Notices& notices);

    void chooseAccount(std::int64_t chosenId);
    void chooseKind(std::int64_t kind);
    void editLimit(std::string text);
    /// @brief Issues a card on the selected account; a blank or unparseable limit issues it with none.
    void issue();
    /// @brief Freezes an active card and unfreezes a frozen one; a cancelled card is left alone.
    void toggleFreeze(std::int64_t cardId);
    void cancel(std::int64_t cardId);
    void refresh();

    [[nodiscard]] static std::vector<Choice> const& kindChoices();

    [[nodiscard]] issuing::State const& form() const noexcept { return _form.state(); }
    [[nodiscard]] std::vector<Choice> const& accountChoices() const { return _accountChoices.get(); }
    [[nodiscard]] std::int64_t selectedAccount() const { return _account.get(); }
    [[nodiscard]] std::vector<CardRow> const& cards() const { return _rows.get(); }
    [[nodiscard]] bool canIssue() const { return _canIssue.get(); }
    /// @brief Whether a card list has arrived for the current session. Tracked.
    [[nodiscard]] bool loaded() const { return _cardList.value().has_value(); }

private:
    morph::reactive::Runtime* _rt;
    morph::reactive::Store<issuing::State, issuing::Msg> _form;
    morph::reactive::Query<dto::ListAccounts> _accounts;
    morph::reactive::Query<dto::ListCards> _cardList;
    morph::reactive::Mutation<dto::IssueCard> _issue;
    morph::reactive::Mutation<dto::FreezeCard> _freeze;
    morph::reactive::Mutation<dto::UnfreezeCard> _unfreeze;
    morph::reactive::Mutation<dto::CancelCard> _cancel;
    morph::reactive::Computed<std::vector<Choice>> _accountChoices;
    morph::reactive::Computed<std::int64_t> _account;
    morph::reactive::Computed<std::vector<CardRow>> _rows;
    morph::reactive::Computed<bool> _canIssue;
    FailureReporter _failures;
};

}  // namespace bank::client
```

Create `examples/bank/app/controllers/cards_controller.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include "controllers/cards_controller.hpp"

#include <optional>
#include <string_view>
#include <utility>

#include "account_choices.hpp"
#include "bank/core/types.hpp"
#include "controllers/auth_controller.hpp"
#include "format.hpp"

namespace bank::client {

namespace issuing {

State init(morph::reactive::Runtime& runtime) {
    return State{.account{runtime, 0}, .kind{runtime, 0}, .limit{runtime, std::string{}}};
}

void Update::operator()(State& state, ChooseAccount const& msg) const { state.account.set(msg.id); }
void Update::operator()(State& state, ChooseKind const& msg) const { state.kind.set(msg.kind); }
void Update::operator()(State& state, EditLimit const& msg) const { state.limit.set(msg.text); }
void Update::operator()(State& state, ClearLimit const& /*msg*/) const { state.limit.set(std::string{}); }

}  // namespace issuing

namespace {

// The model keeps a limit without a currency; it is shown in USD.
CardRow rowOf(dto::CardInfo const& card) {
    auto const status = static_cast<bank::CardStatus>(card.status);
    std::string const kind = card.kind == static_cast<int>(bank::CardKind::Credit) ? "Credit" : "Debit";
    CardRow row{
        .id = card.id,
        .title = kind + " card  ••••" + card.panLast4,
        .limitText = "Daily limit " + fmt::money(card.dailyLimitMinor, 0),
        .statusText = "Cancelled",
        .statusTone = Tone::Bad,
        .active = status == bank::CardStatus::Active,
        .cancelled = status == bank::CardStatus::Cancelled,
        .toggleLabel = status == bank::CardStatus::Active ? "Freeze" : "Unfreeze",
    };
    if (status == bank::CardStatus::Active) {
        row.statusText = "Active";
        row.statusTone = Tone::Good;
    } else if (status == bank::CardStatus::Frozen) {
        row.statusText = "Frozen";
        row.statusTone = Tone::Warn;
    }
    return row;
}

}  // namespace

CardsController::CardsController(morph::reactive::Runtime& runtime, BankHandlers& handlers, AuthController const& auth,
                                 Notices& notices)
    : _rt{&runtime},
      _form{runtime, &issuing::init, issuing::Update{}},
      _accounts{runtime, handlers.customers,
                [session = &auth]() -> std::optional<dto::ListAccounts> {
                    if (!session->signedIn()) {
                        return std::nullopt;
                    }
                    return dto::ListAccounts{};
                }},
      _cardList{runtime, handlers.cards,
                [session = &auth]() -> std::optional<dto::ListCards> {
                    if (!session->signedIn()) {
                        return std::nullopt;
                    }
                    return dto::ListCards{};
                }},
      _issue{runtime, handlers.cards, morph::reactive::MutationOptions{.invalidates = {&_cardList}}},
      _freeze{runtime, handlers.cards, morph::reactive::MutationOptions{.invalidates = {&_cardList}}},
      _unfreeze{runtime, handlers.cards, morph::reactive::MutationOptions{.invalidates = {&_cardList}}},
      _cancel{runtime, handlers.cards, morph::reactive::MutationOptions{.invalidates = {&_cardList}}},
      _accountChoices{runtime, [this] { return openAccountChoices(_accounts.value()); }},
      _account{runtime, [this] { return resolveChoice(_accountChoices.get(), _form.state().account.get()); }},
      _rows{runtime,
            [this] {
                std::vector<CardRow> rows;
                if (std::optional<dto::CardList> const& list = _cardList.value(); list.has_value()) {
                    rows.reserve(list->cards.size());
                    for (dto::CardInfo const& card : list->cards) {
                        rows.push_back(rowOf(card));
                    }
                }
                return rows;
            }},
      _canIssue{runtime, [this] { return _account.get() != 0; }},
      _failures{runtime, notices,
                {[this] { return _accounts.error(); }, [this] { return _cardList.error(); },
                 [this] { return _issue.error(); }, [this] { return _freeze.error(); },
                 [this] { return _unfreeze.error(); }, [this] { return _cancel.error(); }}} {}

void CardsController::chooseAccount(std::int64_t chosenId) { _form.send(issuing::ChooseAccount{chosenId}); }
void CardsController::chooseKind(std::int64_t kind) { _form.send(issuing::ChooseKind{kind}); }
void CardsController::editLimit(std::string text) { _form.send(issuing::EditLimit{std::move(text)}); }

void CardsController::issue() {
    if (!_canIssue.peek()) {
        return;
    }
    issuing::State const& form = _form.state();
    std::string_view const limit = fmt::trimmed(form.limit.peek());
    std::int64_t const minor = limit.empty() ? 0 : fmt::parseMinor(limit).value_or(0);
    _issue.run(dto::IssueCard{
        .accountId = _account.peek(), .kind = static_cast<int>(form.kind.peek()), .dailyLimitMinor = minor});
    _form.send(issuing::ClearLimit{});
}

void CardsController::toggleFreeze(std::int64_t cardId) {
    for (CardRow const& row : _rows.peek()) {
        if (row.id != cardId || row.cancelled) {
            continue;
        }
        if (row.active) {
            _freeze.run(dto::FreezeCard{.id = cardId});
        } else {
            _unfreeze.run(dto::UnfreezeCard{.id = cardId});
        }
        return;
    }
}

void CardsController::cancel(std::int64_t cardId) { _cancel.run(dto::CancelCard{.id = cardId}); }

void CardsController::refresh() {
    _rt->batch([this] {
        _accounts.refetch();
        _cardList.refetch();
    });
}

std::vector<Choice> const& CardsController::kindChoices() {
    static std::vector<Choice> const choices{{.id = 0, .label = "Debit"}, {.id = 1, .label = "Credit"}};
    return choices;
}

}  // namespace bank::client
```

Create `examples/bank/app/views/cards_view.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/ui/view.hpp>

#include "controllers/cards_controller.hpp"

namespace bank::client::views {

/// @brief The cards screen: the issue form and one panel per card.
/// @param cards Must outlive the mounted view.
/// @return The view.
[[nodiscard]] morph::ui::Node cardsView(CardsController& cards);

}  // namespace bank::client::views
```

Create `examples/bank/app/views/cards_view.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include "views/cards_view.hpp"

#include <morph/reactive/signal.hpp>
#include <string>
#include <utility>

#include "views/style.hpp"

namespace bank::client::views {

namespace ui = morph::ui;

ui::Node cardsView(CardsController& cards) {
    CardsController* const ctl = &cards;
    issuing::State const* const form = &cards.form();

    ui::Node issuer = ui::panel({
        .title = "Issue card",
        .padding = 1,
        .child = ui::row({
            .children = {ui::select({.options = [ctl] { return optionsOf(ctl->accountChoices()); },
                                     .selected = [ctl] { return accountKeyOf(ctl->selectedAccount()); },
                                     .onSelect = [ctl](ui::Key key) { ctl->chooseAccount(idOf(key)); }}),
                         ui::select({.options = optionsOf(CardsController::kindChoices()),
                                     .selected = [form] { return keyOf(form->kind.get()); },
                                     .onSelect = [ctl](ui::Key key) { ctl->chooseKind(idOf(key)); }}),
                         ui::textInput({.value = [form] { return form->limit.get(); },
                                        .onChange = [ctl](std::string text) { ctl->editLimit(std::move(text)); },
                                        .placeholder = "Daily limit (opt.)"}),
                         ui::button({.label = "Issue",
                                     .onClick = [ctl] { ctl->issue(); },
                                     .common = {.enabled = [ctl] { return ctl->canIssue(); }}})},
            .gap = 1,
        }),
    });

    ui::Node list = ui::forEach<CardRow>(
        [ctl] { return ctl->cards(); }, [](CardRow const& row) { return ui::Key{row.id}; },
        [ctl](morph::reactive::Signal<CardRow> const& card) {
            auto const* const row = &card;
            auto const live = [row] { return !row->get().cancelled; };
            return ui::panel({
                .padding = 1,
                .child = ui::row({
                    .children = {ui::column({.children = {ui::text({.text = [row] { return row->get().title; },
                                                                    .role = ui::TextRole::Heading}),
                                                          ui::text({.text = [row] { return row->get().limitText; },
                                                                    .role = ui::TextRole::Muted})}}),
                                 ui::spacer({.common = {.layout = {.width = ui::Sizing::stretch()}}}),
                                 ui::text({.text = [row] { return row->get().statusText; },
                                           .role = [row] { return roleOf(row->get().statusTone); }}),
                                 ui::button({.label = [row] { return row->get().toggleLabel; },
                                             .onClick = [ctl, row] { ctl->toggleFreeze(row->peek().id); },
                                             .common = {.visible = live}}),
                                 ui::button({.label = "Cancel",
                                             .onClick = [ctl, row] { ctl->cancel(row->peek().id); },
                                             .common = {.visible = live}})},
                    .gap = 1,
                }),
            });
        },
        ui::Axis::Vertical, 1);

    return ui::column({
        .children = {issuer, ui::scroll({.child = list, .common = {.layout = {.height = ui::Sizing::stretch()}}})},
        .gap = 1,
    });
}

}  // namespace bank::client::views
```

- [ ] **Step 4: Generate the golden, then run the tests to verify they pass**

```bash
cmake --build build/bank --target bank_app_tests
BANK_UPDATE_GOLDEN=1 ./build/bank/examples/bank/bank_app_tests "Cards view: issuing, freezing and cancelling through the screen"
cat examples/bank/tests/app/golden/cards.txt
```

Review it: a Panel `Issue card` holding a Select with `options=[] selected=none`, a Select `Debit`/`Credit` with
`Debit` selected, a TextInput `Daily limit (opt.)` and a Button `Issue` with `enabled=false`; a `Scroll` holding the
list, an empty `Column`.

Run: `./build/bank/examples/bank/bank_app_tests "[bank][app]"`
Expected: PASS — 37 test cases.

Mutation checks, each restored afterwards:
1. In `toggleFreeze`, run `_freeze` whatever `row.active` says. Expected FAIL in "an issued card is listed with its
   limit and can be frozen, unfrozen and cancelled" (times out waiting for `Active` again).
2. Remove `&_cardList` from `_issue`'s `invalidates`. Expected FAIL in the same test (times out waiting for one card).

- [ ] **Step 5: Commit**

```bash
git add examples/bank/app examples/bank/tests/app examples/bank/CMakeLists.txt
git commit -m "wip(bank): CardsController and the cards screen

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 6: `PayeesController` and the payees & bills screen

The old `PayeeController` + `PayeesPage.qml`: add and remove payees, and pay a bill. A payment reloads nothing,
so `Bill paid` in the notice strip is its only confirmation — the half of the old toast test that is about payees.

**Files:**
- Create: `examples/bank/app/controllers/payees_controller.hpp`, `examples/bank/app/controllers/payees_controller.cpp`
- Create: `examples/bank/app/views/payees_view.hpp`, `examples/bank/app/views/payees_view.cpp`
- Create: `examples/bank/tests/app/golden/payees.txt` (generated in Step 4)
- Modify: `examples/bank/CMakeLists.txt` — append `app/controllers/payees_controller.cpp` and
  `app/views/payees_view.cpp` to `BANK_APP_SOURCES`; append `tests/app/test_payees.cpp` to `bank_app_tests`
- Test: `examples/bank/tests/app/test_payees.cpp`

**Interfaces:**
- Consumes: Tasks 1–5 (`openAccountChoices`, `confirmed`); `dto::{AddPayee, RemovePayee, ListPayees, PayeeList,
  PayeeInfo}` (`include/bank/dto/payee_dto.hpp`), `dto::{PayBill, PaymentInfo}` (`payment_dto.hpp`).
- Produces: `namespace bank::client::payee { State{name, iban, bankName, account, payee, amount}; EditName, EditIban,
  EditBankName, ClearPayee, ChooseAccount, ChoosePayee, EditAmount, ClearAmount; Msg; init; Update; }`;
  `struct PayeeRow`; `class PayeesController { PayeesController(Runtime&, BankHandlers&, AuthController const&,
  Notices&); editName, editIban, editBankName, addPayee(), removePayee(id), chooseAccount, choosePayee, editAmount,
  payBill(), refresh(); form(), accountChoices(), payeeChoices(), selectedAccount(), selectedPayee(), payees(),
  canAdd(), canPay(), loaded(); }`; `views::payeesView(PayeesController&)`.

- [ ] **Step 1: Write the failing tests**

Create `examples/bank/tests/app/test_payees.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <cstdint>

#include "bank_app_test_support.hpp"
#include "controllers/payees_controller.hpp"
#include "recorded.hpp"
#include "views/payees_view.hpp"

using bank::client::PayeesController;
using bank::client::Tone;
using bank::testing::BankWiring;
using bank::testing::RecordingBackend;
using bank::testing::settle;

namespace {

constexpr char const* kIban = "DE89370400440532013000";

}  // namespace

TEST_CASE("PayeesController: an added payee is listed and can be removed", "[bank][app][payees]") {
    BankWiring wiring;
    wiring.signUp("payees-add");
    PayeesController payees{wiring.runtime(), wiring.handlers, wiring.auth, wiring.notices};
    REQUIRE(settle(wiring.ctx, [&payees] { return payees.loaded(); }));
    payees.editName("City Power");
    payees.editIban(std::string{" "} + kIban + " ");
    payees.editBankName("Stadtbank");
    REQUIRE(payees.canAdd());
    payees.addPayee();
    CHECK(payees.form().name.peek().empty());
    CHECK(payees.form().iban.peek().empty());
    REQUIRE(settle(wiring.ctx, [&payees] { return payees.payees().size() == 1; }));
    CHECK(payees.payees().front().name == "City Power");
    CHECK(payees.payees().front().iban == kIban);
    CHECK(payees.selectedPayee() == payees.payees().front().id);

    payees.removePayee(payees.payees().front().id);
    REQUIRE(settle(wiring.ctx, [&payees] { return payees.payees().empty(); }));
    CHECK(payees.selectedPayee() == 0);
}

TEST_CASE("PayeesController: adding needs a name and a plausible IBAN", "[bank][app][payees]") {
    BankWiring wiring;
    wiring.signUp("payees-valid");
    PayeesController payees{wiring.runtime(), wiring.handlers, wiring.auth, wiring.notices};
    CHECK_FALSE(payees.canAdd());
    payees.editName("Gas");
    payees.editIban("not-an-iban");
    CHECK_FALSE(payees.canAdd());
    payees.editIban(kIban);
    CHECK(payees.canAdd());
    payees.editName("");
    CHECK_FALSE(payees.canAdd());
}

TEST_CASE("PayeesController: paying a bill confirms it and debits the account", "[bank][app][payees]") {
    BankWiring wiring;
    wiring.signUp("payees-pay");
    std::int64_t const account = wiring.openAccount(0, 0);
    wiring.depositInto(account, 5000);
    PayeesController payees{wiring.runtime(), wiring.handlers, wiring.auth, wiring.notices};
    payees.editName("City Power");
    payees.editIban(kIban);
    payees.addPayee();
    REQUIRE(settle(wiring.ctx,
                   [&payees] { return payees.payees().size() == 1 && payees.accountChoices().size() == 1; }));
    CHECK_FALSE(payees.canPay());  // no amount yet
    payees.editAmount("12.50");
    REQUIRE(payees.canPay());
    payees.payBill();
    CHECK(payees.form().amount.peek().empty());
    REQUIRE(settle(wiring.ctx, [&wiring] { return wiring.notices.visible(); }));
    CHECK(wiring.notices.text() == "Bill paid");
    CHECK(wiring.notices.tone() == Tone::Good);
    CHECK(wiring.balanceOf(account) == 3750);
}

TEST_CASE("Payees view: adding a payee and paying it through the screen", "[bank][app][view][payees]") {
    BankWiring wiring;
    wiring.signUp("view-payees");
    PayeesController payees{wiring.runtime(), wiring.handlers, wiring.auth, wiring.notices};
    REQUIRE(settle(wiring.ctx, [&payees] { return payees.loaded(); }));
    RecordingBackend rec;
    auto const mounted = bank::testing::mount(wiring.ctx, rec, bank::client::views::payeesView(payees));
    bank::testing::checkGolden("payees", rec.dump());

    std::int64_t const account = wiring.openAccount(0, 0);
    wiring.depositInto(account, 2000);
    payees.refresh();
    rec.edit(bank::testing::input(rec, "Payee name"), "Water");
    rec.edit(bank::testing::input(rec, "IBAN"), kIban);
    bank::testing::drain(wiring.ctx);
    rec.click(bank::testing::button(rec, "Add payee"));
    REQUIRE(settle(wiring.ctx,
                   [&payees] { return payees.payees().size() == 1 && payees.accountChoices().size() == 1; }));
    bank::testing::drain(wiring.ctx);
    CHECK(bank::testing::shows(rec, "Water"));

    rec.edit(bank::testing::input(rec, "Amount"), "5.00");
    bank::testing::drain(wiring.ctx);
    rec.click(bank::testing::button(rec, "Pay"));
    REQUIRE(settle(wiring.ctx, [&wiring] { return wiring.notices.visible(); }));
    CHECK(wiring.notices.text() == "Bill paid");
    CHECK(wiring.balanceOf(account) == 1500);
}
```

- [ ] **Step 2: Run them to verify they fail**

Run: `cmake --build build/bank --target bank_app_tests`
Expected: FAIL — `fatal error: 'controllers/payees_controller.hpp' file not found`.

- [ ] **Step 3: Implement**

Create `examples/bank/app/controllers/payees_controller.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <morph/core/callback_scope.hpp>
#include <morph/reactive/control.hpp>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/signal.hpp>
#include <morph/reactive/store.hpp>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "bank/dto/account_dto.hpp"
#include "bank/dto/payee_dto.hpp"
#include "bank/dto/payment_dto.hpp"
#include "bank_handlers.hpp"
#include "choice.hpp"
#include "notices.hpp"

/// @file
/// The payees & bills screen: saved payees, adding and removing one, and paying a bill to one.

namespace bank::client {

class AuthController;

/// @brief The add-payee and pay-bill forms' user intent.
namespace payee {

struct State {
    morph::reactive::Signal<std::string> name;      ///< the new payee's name
    morph::reactive::Signal<std::string> iban;      ///< the new payee's IBAN, as typed
    morph::reactive::Signal<std::string> bankName;  ///< optional
    morph::reactive::Signal<std::int64_t> account;  ///< the paying account the user picked; 0 for none
    morph::reactive::Signal<std::int64_t> payee;    ///< the payee the user picked; 0 for none
    morph::reactive::Signal<std::string> amount;    ///< the bill amount, as typed
};

struct EditName {
    std::string text;
};
struct EditIban {
    std::string text;
};
struct EditBankName {
    std::string text;
};
struct ClearPayee {};
struct ChooseAccount {
    std::int64_t id = 0;
};
struct ChoosePayee {
    std::int64_t id = 0;
};
struct EditAmount {
    std::string text;
};
struct ClearAmount {};

using Msg = std::variant<EditName, EditIban, EditBankName, ClearPayee, ChooseAccount, ChoosePayee, EditAmount,
                         ClearAmount>;

[[nodiscard]] State init(morph::reactive::Runtime& runtime);

struct Update {
    void operator()(State& state, EditName const& msg) const;
    void operator()(State& state, EditIban const& msg) const;
    void operator()(State& state, EditBankName const& msg) const;
    void operator()(State& state, ClearPayee const& msg) const;
    void operator()(State& state, ChooseAccount const& msg) const;
    void operator()(State& state, ChoosePayee const& msg) const;
    void operator()(State& state, EditAmount const& msg) const;
    void operator()(State& state, ClearAmount const& msg) const;
};

}  // namespace payee

/// @brief One saved payee as the screen shows it.
struct PayeeRow {
    std::int64_t id = 0;
    std::string name;
    std::string iban;
    bool operator==(PayeeRow const&) const = default;
};

/// @brief The payees screen's state and commands.
class PayeesController {
public:
    PayeesController(morph::reactive::Runtime& runtime, BankHandlers& handlers, AuthController const& auth,
                     Notices& notices);

    void editName(std::string text);
    void editIban(std::string text);
    void editBankName(std::string text);
    /// @brief Saves the payee, the IBAN trimmed; does nothing while `canAdd()` is false.
    void addPayee();
    void removePayee(std::int64_t payeeId);
    void chooseAccount(std::int64_t chosenId);
    void choosePayee(std::int64_t chosenId);
    void editAmount(std::string text);
    /// @brief Pays the amount to the selected payee; does nothing while `canPay()` is false.
    void payBill();
    void refresh();

    [[nodiscard]] payee::State const& form() const noexcept { return _form.state(); }
    [[nodiscard]] std::vector<Choice> const& accountChoices() const { return _accountChoices.get(); }
    [[nodiscard]] std::vector<Choice> const& payeeChoices() const { return _payeeChoices.get(); }
    [[nodiscard]] std::int64_t selectedAccount() const { return _account.get(); }
    [[nodiscard]] std::int64_t selectedPayee() const { return _payee.get(); }
    [[nodiscard]] std::vector<PayeeRow> const& payees() const { return _rows.get(); }
    [[nodiscard]] bool canAdd() const { return _canAdd.get(); }
    [[nodiscard]] bool canPay() const { return _canPay.get(); }
    [[nodiscard]] bool loaded() const { return _payeeList.value().has_value(); }

private:
    morph::reactive::Runtime* _rt;
    morph::reactive::Store<payee::State, payee::Msg> _form;
    morph::reactive::Query<dto::ListAccounts> _accounts;
    morph::reactive::Query<dto::ListPayees> _payeeList;
    morph::reactive::Mutation<dto::AddPayee> _add;
    morph::reactive::Mutation<dto::RemovePayee> _remove;
    morph::reactive::Mutation<dto::PayBill> _pay;
    morph::reactive::Computed<std::vector<Choice>> _accountChoices;
    morph::reactive::Computed<std::vector<PayeeRow>> _rows;
    morph::reactive::Computed<std::vector<Choice>> _payeeChoices;
    morph::reactive::Computed<std::int64_t> _account;
    morph::reactive::Computed<std::int64_t> _payee;
    morph::reactive::Computed<std::optional<std::int64_t>> _amountMinor;
    morph::reactive::Computed<bool> _canAdd;
    morph::reactive::Computed<bool> _canPay;
    FailureReporter _failures;
    // Last member, so it is destroyed first: a confirmation still in flight is dropped.
    morph::async::CallbackScope _confirmations;
};

}  // namespace bank::client
```

Create `examples/bank/app/controllers/payees_controller.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include "controllers/payees_controller.hpp"

#include <string_view>
#include <utility>

#include "account_choices.hpp"
#include "controllers/auth_controller.hpp"
#include "format.hpp"

namespace bank::client {

namespace payee {

State init(morph::reactive::Runtime& runtime) {
    return State{.name{runtime, std::string{}},
                 .iban{runtime, std::string{}},
                 .bankName{runtime, std::string{}},
                 .account{runtime, 0},
                 .payee{runtime, 0},
                 .amount{runtime, std::string{}}};
}

void Update::operator()(State& state, EditName const& msg) const { state.name.set(msg.text); }
void Update::operator()(State& state, EditIban const& msg) const { state.iban.set(msg.text); }
void Update::operator()(State& state, EditBankName const& msg) const { state.bankName.set(msg.text); }

void Update::operator()(State& state, ClearPayee const& /*msg*/) const {
    state.name.set(std::string{});
    state.iban.set(std::string{});
    state.bankName.set(std::string{});
}

void Update::operator()(State& state, ChooseAccount const& msg) const { state.account.set(msg.id); }
void Update::operator()(State& state, ChoosePayee const& msg) const { state.payee.set(msg.id); }
void Update::operator()(State& state, EditAmount const& msg) const { state.amount.set(msg.text); }
void Update::operator()(State& state, ClearAmount const& /*msg*/) const { state.amount.set(std::string{}); }

}  // namespace payee

namespace {

dto::AddPayee draftOf(std::string const& name, std::string_view iban, std::string const& bankName) {
    return dto::AddPayee{.name = name, .iban = std::string{fmt::trimmed(iban)}, .bankName = bankName};
}

}  // namespace

PayeesController::PayeesController(morph::reactive::Runtime& runtime, BankHandlers& handlers,
                                   AuthController const& auth, Notices& notices)
    : _rt{&runtime},
      _form{runtime, &payee::init, payee::Update{}},
      _accounts{runtime, handlers.customers,
                [session = &auth]() -> std::optional<dto::ListAccounts> {
                    if (!session->signedIn()) {
                        return std::nullopt;
                    }
                    return dto::ListAccounts{};
                }},
      _payeeList{runtime, handlers.payees,
                 [session = &auth]() -> std::optional<dto::ListPayees> {
                     if (!session->signedIn()) {
                         return std::nullopt;
                     }
                     return dto::ListPayees{};
                 }},
      _add{runtime, handlers.payees, morph::reactive::MutationOptions{.invalidates = {&_payeeList}}},
      _remove{runtime, handlers.payees, morph::reactive::MutationOptions{.invalidates = {&_payeeList}}},
      _pay{runtime, confirmed<dto::PayBill>(handlers.payments, notices, _confirmations, "Bill paid")},
      _accountChoices{runtime, [this] { return openAccountChoices(_accounts.value()); }},
      _rows{runtime,
            [this] {
                std::vector<PayeeRow> rows;
                if (std::optional<dto::PayeeList> const& list = _payeeList.value(); list.has_value()) {
                    rows.reserve(list->payees.size());
                    for (dto::PayeeInfo const& info : list->payees) {
                        rows.push_back(PayeeRow{.id = info.id, .name = info.name, .iban = info.iban});
                    }
                }
                return rows;
            }},
      _payeeChoices{runtime,
                    [this] {
                        std::vector<Choice> choices;
                        for (PayeeRow const& row : _rows.get()) {
                            choices.push_back(Choice{.id = row.id, .label = row.name});
                        }
                        return choices;
                    }},
      _account{runtime, [this] { return resolveChoice(_accountChoices.get(), _form.state().account.get()); }},
      _payee{runtime, [this] { return resolveChoice(_payeeChoices.get(), _form.state().payee.get()); }},
      _amountMinor{runtime, [this] { return fmt::parseMinor(_form.state().amount.get()); }},
      _canAdd{runtime,
              [this] {
                  payee::State const& form = _form.state();
                  return draftOf(form.name.get(), form.iban.get(), form.bankName.get()).validate();
              }},
      _canPay{runtime, [this] { return _amountMinor.get().has_value() && _account.get() != 0 && _payee.get() != 0; }},
      _failures{runtime, notices,
                {[this] { return _accounts.error(); }, [this] { return _payeeList.error(); },
                 [this] { return _add.error(); }, [this] { return _remove.error(); },
                 [this] { return _pay.error(); }}} {}

void PayeesController::editName(std::string text) { _form.send(payee::EditName{std::move(text)}); }
void PayeesController::editIban(std::string text) { _form.send(payee::EditIban{std::move(text)}); }
void PayeesController::editBankName(std::string text) { _form.send(payee::EditBankName{std::move(text)}); }

void PayeesController::addPayee() {
    if (!_canAdd.peek()) {
        return;
    }
    payee::State const& form = _form.state();
    _add.run(draftOf(form.name.peek(), form.iban.peek(), form.bankName.peek()));
    _form.send(payee::ClearPayee{});
}

void PayeesController::removePayee(std::int64_t payeeId) { _remove.run(dto::RemovePayee{.id = payeeId}); }
void PayeesController::chooseAccount(std::int64_t chosenId) { _form.send(payee::ChooseAccount{chosenId}); }
void PayeesController::choosePayee(std::int64_t chosenId) { _form.send(payee::ChoosePayee{chosenId}); }
void PayeesController::editAmount(std::string text) { _form.send(payee::EditAmount{std::move(text)}); }

void PayeesController::payBill() {
    if (!_canPay.peek()) {
        return;
    }
    _pay.run(dto::PayBill{
        .fromAccountId = _account.peek(), .payeeId = _payee.peek(), .amountMinor = _amountMinor.peek().value_or(0)});
    _form.send(payee::ClearAmount{});
}

void PayeesController::refresh() {
    _rt->batch([this] {
        _accounts.refetch();
        _payeeList.refetch();
    });
}

}  // namespace bank::client
```

Create `examples/bank/app/views/payees_view.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/ui/view.hpp>

#include "controllers/payees_controller.hpp"

namespace bank::client::views {

/// @brief The payees & bills screen: the add form, the pay form, one panel per payee.
/// @param payees Must outlive the mounted view.
/// @return The view.
[[nodiscard]] morph::ui::Node payeesView(PayeesController& payees);

}  // namespace bank::client::views
```

Create `examples/bank/app/views/payees_view.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include "views/payees_view.hpp"

#include <morph/reactive/signal.hpp>
#include <string>
#include <utility>

#include "views/style.hpp"

namespace bank::client::views {

namespace ui = morph::ui;

ui::Node payeesView(PayeesController& payees) {
    PayeesController* const ctl = &payees;
    payee::State const* const form = &payees.form();
    ui::Common const stretch{.layout = {.width = ui::Sizing::stretch()}};

    ui::Node adder = ui::panel({
        .padding = 1,
        .child = ui::row({
            .children = {ui::textInput({.value = [form] { return form->name.get(); },
                                        .onChange = [ctl](std::string text) { ctl->editName(std::move(text)); },
                                        .placeholder = "Payee name",
                                        .common = stretch}),
                         ui::textInput({.value = [form] { return form->iban.get(); },
                                        .onChange = [ctl](std::string text) { ctl->editIban(std::move(text)); },
                                        .placeholder = "IBAN",
                                        .common = stretch}),
                         ui::textInput({.value = [form] { return form->bankName.get(); },
                                        .onChange = [ctl](std::string text) { ctl->editBankName(std::move(text)); },
                                        .placeholder = "Bank (optional)",
                                        .common = stretch}),
                         ui::button({.label = "Add payee",
                                     .onClick = [ctl] { ctl->addPayee(); },
                                     .common = {.enabled = [ctl] { return ctl->canAdd(); }}})},
            .gap = 1,
        }),
    });

    ui::Node payer = ui::panel({
        .title = "Pay bill",
        .padding = 1,
        .child = ui::row({
            .children = {ui::select({.options = [ctl] { return optionsOf(ctl->accountChoices()); },
                                     .selected = [ctl] { return accountKeyOf(ctl->selectedAccount()); },
                                     .onSelect = [ctl](ui::Key key) { ctl->chooseAccount(idOf(key)); }}),
                         ui::select({.options = [ctl] { return optionsOf(ctl->payeeChoices()); },
                                     .selected = [ctl] { return accountKeyOf(ctl->selectedPayee()); },
                                     .onSelect = [ctl](ui::Key key) { ctl->choosePayee(idOf(key)); }}),
                         ui::textInput({.value = [form] { return form->amount.get(); },
                                        .onChange = [ctl](std::string text) { ctl->editAmount(std::move(text)); },
                                        .placeholder = "Amount"}),
                         ui::button({.label = "Pay",
                                     .onClick = [ctl] { ctl->payBill(); },
                                     .common = {.enabled = [ctl] { return ctl->canPay(); }}})},
            .gap = 1,
        }),
    });

    ui::Node list = ui::forEach<PayeeRow>(
        [ctl] { return ctl->payees(); }, [](PayeeRow const& row) { return ui::Key{row.id}; },
        [ctl](morph::reactive::Signal<PayeeRow> const& entry) {
            auto const* const row = &entry;
            return ui::panel({
                .padding = 1,
                .child = ui::row({
                    .children = {ui::column({.children = {ui::text({.text = [row] { return row->get().name; },
                                                                    .role = ui::TextRole::Heading}),
                                                          ui::text({.text = [row] { return row->get().iban; },
                                                                    .role = ui::TextRole::Muted})}}),
                                 ui::spacer({.common = {.layout = {.width = ui::Sizing::stretch()}}}),
                                 ui::button({.label = "Remove",
                                             .onClick = [ctl, row] { ctl->removePayee(row->peek().id); }})},
                    .gap = 1,
                }),
            });
        },
        ui::Axis::Vertical, 1);

    return ui::column({
        .children = {adder, payer,
                     ui::scroll({.child = list, .common = {.layout = {.height = ui::Sizing::stretch()}}})},
        .gap = 1,
    });
}

}  // namespace bank::client::views
```

- [ ] **Step 4: Generate the golden, then run the tests to verify they pass**

```bash
cmake --build build/bank --target bank_app_tests
BANK_UPDATE_GOLDEN=1 ./build/bank/examples/bank/bank_app_tests "Payees view: adding a payee and paying it through the screen"
cat examples/bank/tests/app/golden/payees.txt
```

Review it: a Panel with TextInputs `Payee name`, `IBAN`, `Bank (optional)` and a Button `Add payee`
(`enabled=false`); a Panel `Pay bill` with two Selects showing `options=[] selected=none`, a TextInput `Amount` and
a Button `Pay` (`enabled=false`); a `Scroll` holding the list, an empty `Column`.

Run: `./build/bank/examples/bank/bank_app_tests "[bank][app]"`
Expected: PASS — 41 test cases.

Mutation checks, each restored afterwards:
1. Construct `_pay` as `_pay{runtime, handlers.payments}` (no confirmation). Expected FAIL in "paying a bill confirms it
   and debits the account" (times out waiting for the notice).
2. In `draftOf`, pass `std::string{iban}` instead of the trimmed IBAN. Expected FAIL in "an added payee is listed and
   can be removed" at `REQUIRE(payees.canAdd())`.

- [ ] **Step 5: Commit**

```bash
git add examples/bank/app examples/bank/tests/app examples/bank/CMakeLists.txt
git commit -m "wip(bank): PayeesController and the payees & bills screen

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 7: `LoansController` and the loans screen

The old `LoanController` + `LoansPage.qml`: apply for a loan, repay one in full, and show a loan's amortization
schedule. The schedule is a `Query` keyed on the loan being shown, so it gets latest-wins for free — the same fix
as the history's. The QML's rate/term parsing (`parseInt`, `-1` for a blank rate) moves into `Computed`s over
`fmt::parseWhole`; the repay amount is the outstanding balance in minor units, with no round trip through text.

**Files:**
- Create: `examples/bank/app/controllers/loans_controller.hpp`, `examples/bank/app/controllers/loans_controller.cpp`
- Create: `examples/bank/app/views/loans_view.hpp`, `examples/bank/app/views/loans_view.cpp`
- Create: `examples/bank/tests/app/golden/loans.txt` (generated in Step 4)
- Modify: `examples/bank/CMakeLists.txt` — append `app/controllers/loans_controller.cpp` and `app/views/loans_view.cpp`
  to `BANK_APP_SOURCES`; append `tests/app/test_loans.cpp` to `bank_app_tests`
- Test: `examples/bank/tests/app/test_loans.cpp`

**Interfaces:**
- Consumes: Tasks 1–6; `dto::{ApplyLoan, RepayLoan, ListLoans, LoanList, LoanInfo, LoanScheduleRequest,
  LoanScheduleResult, Installment}` (`include/bank/dto/loan_dto.hpp`), `bank::LoanStatus`.
- Produces: `namespace bank::client::lending { State{account, principal, rate, term, scheduleLoan}; ChooseAccount,
  EditPrincipal, EditRate, EditTerm, ClearApplication, ShowSchedule; Msg; init; Update; }`; `struct LoanRow`;
  `struct InstallmentRow`; `class LoansController { LoansController(Runtime&, BankHandlers&, AuthController const&,
  Notices&); chooseAccount, editPrincipal, editRate, editTerm, apply(), repay(loanId), showSchedule(loanId),
  refresh(); form(), accountChoices(), selectedAccount(), loans(), schedule(), canApply(), loaded(); }`;
  `views::loansView(LoansController&)`.

- [ ] **Step 1: Write the failing tests**

Create `examples/bank/tests/app/test_loans.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <string>
#include <utility>

#include "bank_app_test_support.hpp"
#include "controllers/loans_controller.hpp"
#include "recorded.hpp"
#include "views/loans_view.hpp"

using bank::client::LoanRow;
using bank::client::LoansController;
using bank::client::Tone;
using bank::testing::BankWiring;
using bank::testing::RecordingBackend;
using bank::testing::settle;

namespace {

void applyFor(LoansController& loans, std::string principal, std::string rate, std::string term) {
    loans.editPrincipal(std::move(principal));
    loans.editRate(std::move(rate));
    loans.editTerm(std::move(term));
    REQUIRE(loans.canApply());
    loans.apply();
}

}  // namespace

TEST_CASE("LoansController: an approved loan is listed with its terms", "[bank][app][loans]") {
    BankWiring wiring;
    wiring.signUp("loans-apply");
    std::int64_t const account = wiring.openAccount(0, 0);
    LoansController loans{wiring.runtime(), wiring.handlers, wiring.auth, wiring.notices};
    REQUIRE(settle(wiring.ctx, [&loans] { return loans.accountChoices().size() == 1 && loans.loaded(); }));
    CHECK(loans.selectedAccount() == account);

    applyFor(loans, "120.00", "600", "12");
    CHECK(loans.form().principal.peek().empty());
    REQUIRE(settle(wiring.ctx, [&loans] { return loans.loans().size() == 1; }));
    LoanRow const loan = loans.loans().front();
    CHECK(loan.title == "Loan #" + std::to_string(loan.id));
    CHECK(loan.detail == "Outstanding 120.00 USD  ·  600 bps  ·  12 mo");
    CHECK(loan.statusText == "Active");
    CHECK(loan.statusTone == Tone::Neutral);
    CHECK(loan.active);
    CHECK(wiring.balanceOf(account) == 12000);  // disbursed into the account
}

TEST_CASE("LoansController: the schedule shows one row per month", "[bank][app][loans]") {
    BankWiring wiring;
    wiring.signUp("loans-schedule");
    static_cast<void>(wiring.openAccount(0, 0));
    LoansController loans{wiring.runtime(), wiring.handlers, wiring.auth, wiring.notices};
    REQUIRE(settle(wiring.ctx, [&loans] { return loans.accountChoices().size() == 1; }));
    applyFor(loans, "1200.00", "0", "12");
    REQUIRE(settle(wiring.ctx, [&loans] { return loans.loans().size() == 1; }));
    loans.showSchedule(loans.loans().front().id);
    REQUIRE(settle(wiring.ctx, [&loans] { return loans.schedule().size() == 12; }));
    CHECK(loans.schedule().front().monthText == "1");
    CHECK(loans.schedule().front().principalText == "100.00 USD");
    CHECK(loans.schedule().front().interestText == "0.00 USD");
    CHECK(loans.schedule().back().monthText == "12");
    CHECK(loans.schedule().back().remainingText == "0.00 USD");
}

TEST_CASE("LoansController: repaying the outstanding balance pays the loan off", "[bank][app][loans]") {
    BankWiring wiring;
    wiring.signUp("loans-repay");
    std::int64_t const account = wiring.openAccount(0, 0);
    LoansController loans{wiring.runtime(), wiring.handlers, wiring.auth, wiring.notices};
    REQUIRE(settle(wiring.ctx, [&loans] { return loans.accountChoices().size() == 1; }));
    applyFor(loans, "120.00", "0", "12");
    REQUIRE(settle(wiring.ctx, [&loans] { return loans.loans().size() == 1; }));
    loans.repay(loans.loans().front().id);
    REQUIRE(settle(wiring.ctx, [&loans] { return loans.loans().front().statusText == "Paid off"; }));
    CHECK(loans.loans().front().statusTone == Tone::Good);
    CHECK_FALSE(loans.loans().front().active);
    CHECK(wiring.balanceOf(account) == 0);
}

TEST_CASE("LoansController: rate and term must be whole numbers; a zero rate is allowed", "[bank][app][loans]") {
    BankWiring wiring;
    wiring.signUp("loans-valid");
    static_cast<void>(wiring.openAccount(0, 0));
    LoansController loans{wiring.runtime(), wiring.handlers, wiring.auth, wiring.notices};
    REQUIRE(settle(wiring.ctx, [&loans] { return loans.accountChoices().size() == 1; }));
    loans.editPrincipal("100");
    loans.editTerm("12");
    CHECK_FALSE(loans.canApply());  // a blank rate is not a zero rate
    loans.editRate("0");
    CHECK(loans.canApply());
    loans.editRate("-5");
    CHECK_FALSE(loans.canApply());
    loans.editRate("1.5");
    CHECK_FALSE(loans.canApply());
    loans.editRate("600");
    loans.editTerm("0");
    CHECK_FALSE(loans.canApply());
    loans.editTerm("12");
    loans.editPrincipal("abc");
    CHECK_FALSE(loans.canApply());
}

TEST_CASE("Loans view: applying, showing the schedule and repaying through the screen", "[bank][app][view][loans]") {
    BankWiring wiring;
    wiring.signUp("view-loans");
    LoansController loans{wiring.runtime(), wiring.handlers, wiring.auth, wiring.notices};
    REQUIRE(settle(wiring.ctx, [&loans] { return loans.loaded(); }));
    RecordingBackend rec;
    auto const mounted = bank::testing::mount(wiring.ctx, rec, bank::client::views::loansView(loans));
    bank::testing::checkGolden("loans", rec.dump());

    static_cast<void>(wiring.openAccount(0, 0));
    loans.refresh();
    REQUIRE(settle(wiring.ctx, [&loans] { return loans.accountChoices().size() == 1; }));
    rec.edit(bank::testing::input(rec, "Principal"), "1200.00");
    rec.edit(bank::testing::input(rec, "Rate (bps)"), "0");
    rec.edit(bank::testing::input(rec, "Months"), "12");
    bank::testing::drain(wiring.ctx);
    rec.click(bank::testing::button(rec, "Apply"));
    REQUIRE(settle(wiring.ctx, [&loans] { return loans.loans().size() == 1; }));
    bank::testing::drain(wiring.ctx);
    CHECK(bank::testing::shows(rec, "Loan #" + std::to_string(loans.loans().front().id)));

    rec.click(bank::testing::button(rec, "Schedule"));
    REQUIRE(settle(wiring.ctx, [&loans] { return loans.schedule().size() == 12; }));
    bank::testing::drain(wiring.ctx);
    CHECK(bank::testing::shows(rec, "100.00 USD"));

    rec.click(bank::testing::button(rec, "Repay"));
    REQUIRE(settle(wiring.ctx, [&loans] { return !loans.loans().front().active; }));
    bank::testing::drain(wiring.ctx);
    CHECK(bank::testing::shows(rec, "Paid off"));
    CHECK_FALSE(bank::testing::visible(rec, bank::testing::button(rec, "Repay")));
}
```

- [ ] **Step 2: Run them to verify they fail**

Run: `cmake --build build/bank --target bank_app_tests`
Expected: FAIL — `fatal error: 'controllers/loans_controller.hpp' file not found`.

- [ ] **Step 3: Implement**

Create `examples/bank/app/controllers/loans_controller.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <morph/reactive/control.hpp>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/signal.hpp>
#include <morph/reactive/store.hpp>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "bank/dto/account_dto.hpp"
#include "bank/dto/loan_dto.hpp"
#include "bank_handlers.hpp"
#include "choice.hpp"
#include "notices.hpp"
#include "tone.hpp"

/// @file
/// The loans screen: apply for a loan, repay one in full, and show a loan's amortization schedule.

namespace bank::client {

class AuthController;

/// @brief The loan application form's user intent, and which schedule is shown.
namespace lending {

struct State {
    morph::reactive::Signal<std::int64_t> account;       ///< the user's pick; 0 for none
    morph::reactive::Signal<std::string> principal;      ///< as typed, in major units
    morph::reactive::Signal<std::string> rate;           ///< basis points, as typed; blank is not zero
    morph::reactive::Signal<std::string> term;           ///< months, as typed
    morph::reactive::Signal<std::int64_t> scheduleLoan;  ///< the loan whose schedule is shown; 0 for none
};

struct ChooseAccount {
    std::int64_t id = 0;
};
struct EditPrincipal {
    std::string text;
};
struct EditRate {
    std::string text;
};
struct EditTerm {
    std::string text;
};
struct ClearApplication {};
struct ShowSchedule {
    std::int64_t loanId = 0;
};

using Msg = std::variant<ChooseAccount, EditPrincipal, EditRate, EditTerm, ClearApplication, ShowSchedule>;

[[nodiscard]] State init(morph::reactive::Runtime& runtime);

struct Update {
    void operator()(State& state, ChooseAccount const& msg) const;
    void operator()(State& state, EditPrincipal const& msg) const;
    void operator()(State& state, EditRate const& msg) const;
    void operator()(State& state, EditTerm const& msg) const;
    void operator()(State& state, ClearApplication const& msg) const;
    void operator()(State& state, ShowSchedule const& msg) const;
};

}  // namespace lending

/// @brief One loan as the screen shows it.
struct LoanRow {
    std::int64_t id = 0;
    std::int64_t accountId = 0;    ///< the account it was disbursed into, which repays it
    std::string title;             ///< `Loan #id`
    std::string detail;            ///< outstanding, rate, term
    std::int64_t outstanding = 0;  ///< minor units; what a repayment pays
    std::string statusText;        ///< `Active` or `Paid off`
    Tone statusTone = Tone::Neutral;
    bool active = false;           ///< only an active loan can be repaid
    bool operator==(LoanRow const&) const = default;
};

/// @brief One month of an amortization schedule.
struct InstallmentRow {
    int month = 0;
    std::string monthText;
    std::string principalText;
    std::string interestText;
    std::string remainingText;
    bool operator==(InstallmentRow const&) const = default;
};

/// @brief The loans screen's state and commands.
class LoansController {
public:
    LoansController(morph::reactive::Runtime& runtime, BankHandlers& handlers, AuthController const& auth,
                    Notices& notices);

    void chooseAccount(std::int64_t chosenId);
    void editPrincipal(std::string text);
    void editRate(std::string text);
    void editTerm(std::string text);
    /// @brief Applies for the loan the form describes; does nothing while `canApply()` is false.
    void apply();
    /// @brief Repays an active loan's whole outstanding balance from the account it was paid into.
    void repay(std::int64_t loanId);
    /// @brief Shows @p loanId's schedule, fetching it again when it is already shown.
    void showSchedule(std::int64_t loanId);
    void refresh();

    [[nodiscard]] lending::State const& form() const noexcept { return _form.state(); }
    [[nodiscard]] std::vector<Choice> const& accountChoices() const { return _accountChoices.get(); }
    [[nodiscard]] std::int64_t selectedAccount() const { return _account.get(); }
    [[nodiscard]] std::vector<LoanRow> const& loans() const { return _rows.get(); }
    [[nodiscard]] std::vector<InstallmentRow> const& schedule() const { return _installments.get(); }
    [[nodiscard]] bool canApply() const { return _canApply.get(); }
    [[nodiscard]] bool loaded() const { return _loanList.value().has_value(); }

private:
    morph::reactive::Runtime* _rt;
    morph::reactive::Store<lending::State, lending::Msg> _form;
    morph::reactive::Query<dto::ListAccounts> _accounts;
    morph::reactive::Query<dto::ListLoans> _loanList;
    morph::reactive::Query<dto::LoanScheduleRequest> _schedule;
    morph::reactive::Mutation<dto::ApplyLoan> _apply;
    morph::reactive::Mutation<dto::RepayLoan> _repay;
    morph::reactive::Computed<std::vector<Choice>> _accountChoices;
    morph::reactive::Computed<std::int64_t> _account;
    morph::reactive::Computed<std::vector<LoanRow>> _rows;
    morph::reactive::Computed<std::vector<InstallmentRow>> _installments;
    morph::reactive::Computed<std::optional<std::int64_t>> _principalMinor;
    morph::reactive::Computed<std::optional<int>> _rateBps;
    morph::reactive::Computed<std::optional<int>> _termMonths;
    morph::reactive::Computed<bool> _canApply;
    FailureReporter _failures;
};

}  // namespace bank::client
```

Create `examples/bank/app/controllers/loans_controller.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include "controllers/loans_controller.hpp"

#include <utility>

#include "account_choices.hpp"
#include "bank/core/types.hpp"
#include "controllers/auth_controller.hpp"
#include "format.hpp"

namespace bank::client {

namespace lending {

State init(morph::reactive::Runtime& runtime) {
    return State{.account{runtime, 0},
                 .principal{runtime, std::string{}},
                 .rate{runtime, std::string{}},
                 .term{runtime, std::string{}},
                 .scheduleLoan{runtime, 0}};
}

void Update::operator()(State& state, ChooseAccount const& msg) const { state.account.set(msg.id); }
void Update::operator()(State& state, EditPrincipal const& msg) const { state.principal.set(msg.text); }
void Update::operator()(State& state, EditRate const& msg) const { state.rate.set(msg.text); }
void Update::operator()(State& state, EditTerm const& msg) const { state.term.set(msg.text); }

void Update::operator()(State& state, ClearApplication const& /*msg*/) const {
    state.principal.set(std::string{});
    state.rate.set(std::string{});
    state.term.set(std::string{});
}

void Update::operator()(State& state, ShowSchedule const& msg) const { state.scheduleLoan.set(msg.loanId); }

}  // namespace lending

namespace {

LoanRow rowOf(dto::LoanInfo const& loan) {
    auto const status = static_cast<bank::LoanStatus>(loan.status);
    bool const paid = status == bank::LoanStatus::PaidOff;
    return LoanRow{
        .id = loan.id,
        .accountId = loan.accountId,
        .title = "Loan #" + std::to_string(loan.id),
        .detail = "Outstanding " + fmt::money(loan.outstandingMinor, loan.currency) + "  ·  " +
                  std::to_string(loan.rateBps) + " bps  ·  " + std::to_string(loan.termMonths) + " mo",
        .outstanding = loan.outstandingMinor,
        .statusText = paid ? "Paid off" : "Active",
        .statusTone = paid ? Tone::Good : Tone::Neutral,
        .active = status == bank::LoanStatus::Active,
    };
}

// The schedule carries no currency; its amounts are shown in USD.
InstallmentRow installmentOf(dto::Installment const& installment) {
    return InstallmentRow{
        .month = installment.month,
        .monthText = std::to_string(installment.month),
        .principalText = fmt::money(installment.principalMinor, 0),
        .interestText = fmt::money(installment.interestMinor, 0),
        .remainingText = fmt::money(installment.remainingMinor, 0),
    };
}

}  // namespace

LoansController::LoansController(morph::reactive::Runtime& runtime, BankHandlers& handlers, AuthController const& auth,
                                 Notices& notices)
    : _rt{&runtime},
      _form{runtime, &lending::init, lending::Update{}},
      _accounts{runtime, handlers.customers,
                [session = &auth]() -> std::optional<dto::ListAccounts> {
                    if (!session->signedIn()) {
                        return std::nullopt;
                    }
                    return dto::ListAccounts{};
                }},
      _loanList{runtime, handlers.loans,
                [session = &auth]() -> std::optional<dto::ListLoans> {
                    if (!session->signedIn()) {
                        return std::nullopt;
                    }
                    return dto::ListLoans{};
                }},
      _schedule{runtime, handlers.loans,
                [this, session = &auth]() -> std::optional<dto::LoanScheduleRequest> {
                    std::int64_t const loan = _form.state().scheduleLoan.get();
                    if (!session->signedIn() || loan == 0) {
                        return std::nullopt;
                    }
                    return dto::LoanScheduleRequest{.loanId = loan};
                }},
      _apply{runtime, handlers.loans, morph::reactive::MutationOptions{.invalidates = {&_loanList}}},
      _repay{runtime, handlers.loans, morph::reactive::MutationOptions{.invalidates = {&_loanList}}},
      _accountChoices{runtime, [this] { return openAccountChoices(_accounts.value()); }},
      _account{runtime, [this] { return resolveChoice(_accountChoices.get(), _form.state().account.get()); }},
      _rows{runtime,
            [this] {
                std::vector<LoanRow> rows;
                if (std::optional<dto::LoanList> const& list = _loanList.value(); list.has_value()) {
                    rows.reserve(list->loans.size());
                    for (dto::LoanInfo const& loan : list->loans) {
                        rows.push_back(rowOf(loan));
                    }
                }
                return rows;
            }},
      _installments{runtime,
                    [this] {
                        std::vector<InstallmentRow> rows;
                        if (std::optional<dto::LoanScheduleResult> const& result = _schedule.value();
                            result.has_value()) {
                            rows.reserve(result->installments.size());
                            for (dto::Installment const& installment : result->installments) {
                                rows.push_back(installmentOf(installment));
                            }
                        }
                        return rows;
                    }},
      _principalMinor{runtime, [this] { return fmt::parseMinor(_form.state().principal.get()); }},
      _rateBps{runtime, [this] { return fmt::parseWhole(_form.state().rate.get()); }},
      _termMonths{runtime, [this] { return fmt::parseWhole(_form.state().term.get()); }},
      _canApply{runtime,
                [this] {
                    return _principalMinor.get().has_value() && _account.get() != 0 &&
                           _rateBps.get().value_or(-1) >= 0 && _termMonths.get().value_or(0) > 0;
                }},
      _failures{runtime, notices,
                {[this] { return _accounts.error(); }, [this] { return _loanList.error(); },
                 [this] { return _schedule.error(); }, [this] { return _apply.error(); },
                 [this] { return _repay.error(); }}} {}

void LoansController::chooseAccount(std::int64_t chosenId) { _form.send(lending::ChooseAccount{chosenId}); }
void LoansController::editPrincipal(std::string text) { _form.send(lending::EditPrincipal{std::move(text)}); }
void LoansController::editRate(std::string text) { _form.send(lending::EditRate{std::move(text)}); }
void LoansController::editTerm(std::string text) { _form.send(lending::EditTerm{std::move(text)}); }

void LoansController::apply() {
    if (!_canApply.peek()) {
        return;
    }
    _apply.run(dto::ApplyLoan{.accountId = _account.peek(),
                              .principalMinor = _principalMinor.peek().value_or(0),
                              .rateBps = _rateBps.peek().value_or(0),
                              .termMonths = _termMonths.peek().value_or(0)});
    _form.send(lending::ClearApplication{});
}

void LoansController::repay(std::int64_t loanId) {
    for (LoanRow const& row : _rows.peek()) {
        if (row.id == loanId && row.active) {
            _repay.run(dto::RepayLoan{.loanId = loanId, .fromAccountId = row.accountId, .amountMinor = row.outstanding});
            return;
        }
    }
}

void LoansController::showSchedule(std::int64_t loanId) {
    if (_form.state().scheduleLoan.peek() == loanId) {
        _schedule.refetch();
        return;
    }
    _form.send(lending::ShowSchedule{loanId});
}

void LoansController::refresh() {
    _rt->batch([this] {
        _accounts.refetch();
        _loanList.refetch();
        _schedule.refetch();
    });
}

}  // namespace bank::client
```

Create `examples/bank/app/views/loans_view.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/ui/view.hpp>

#include "controllers/loans_controller.hpp"

namespace bank::client::views {

/// @brief The loans screen: the application form, one panel per loan, the schedule table.
/// @param loans Must outlive the mounted view.
/// @return The view.
[[nodiscard]] morph::ui::Node loansView(LoansController& loans);

}  // namespace bank::client::views
```

Create `examples/bank/app/views/loans_view.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include "views/loans_view.hpp"

#include <cstdint>
#include <morph/reactive/signal.hpp>
#include <string>
#include <utility>
#include <vector>

#include "views/style.hpp"

namespace bank::client::views {

namespace ui = morph::ui;

ui::Node loansView(LoansController& loans) {
    LoansController* const ctl = &loans;
    lending::State const* const form = &loans.form();

    ui::Node applier = ui::panel({
        .title = "Apply for a loan",
        .padding = 1,
        .child = ui::row({
            .children = {ui::select({.options = [ctl] { return optionsOf(ctl->accountChoices()); },
                                     .selected = [ctl] { return accountKeyOf(ctl->selectedAccount()); },
                                     .onSelect = [ctl](ui::Key key) { ctl->chooseAccount(idOf(key)); }}),
                         ui::textInput({.value = [form] { return form->principal.get(); },
                                        .onChange = [ctl](std::string text) { ctl->editPrincipal(std::move(text)); },
                                        .placeholder = "Principal"}),
                         ui::textInput({.value = [form] { return form->rate.get(); },
                                        .onChange = [ctl](std::string text) { ctl->editRate(std::move(text)); },
                                        .placeholder = "Rate (bps)"}),
                         ui::textInput({.value = [form] { return form->term.get(); },
                                        .onChange = [ctl](std::string text) { ctl->editTerm(std::move(text)); },
                                        .placeholder = "Months"}),
                         ui::button({.label = "Apply",
                                     .onClick = [ctl] { ctl->apply(); },
                                     .common = {.enabled = [ctl] { return ctl->canApply(); }}})},
            .gap = 1,
        }),
    });

    ui::Node list = ui::forEach<LoanRow>(
        [ctl] { return ctl->loans(); }, [](LoanRow const& row) { return ui::Key{row.id}; },
        [ctl](morph::reactive::Signal<LoanRow> const& loan) {
            auto const* const row = &loan;
            return ui::panel({
                .padding = 1,
                .child = ui::row({
                    .children = {ui::column({.children = {ui::text({.text = [row] { return row->get().title; },
                                                                    .role = ui::TextRole::Heading}),
                                                          ui::text({.text = [row] { return row->get().detail; },
                                                                    .role = ui::TextRole::Muted})}}),
                                 ui::spacer({.common = {.layout = {.width = ui::Sizing::stretch()}}}),
                                 ui::text({.text = [row] { return row->get().statusText; },
                                           .role = [row] { return roleOf(row->get().statusTone); }}),
                                 ui::button({.label = "Schedule",
                                             .onClick = [ctl, row] { ctl->showSchedule(row->peek().id); }}),
                                 ui::button({.label = "Repay",
                                             .onClick = [ctl, row] { ctl->repay(row->peek().id); },
                                             .common = {.visible = [row] { return row->get().active; }}})},
                    .gap = 1,
                }),
            });
        },
        ui::Axis::Vertical, 1);

    ui::Node schedule = ui::table<InstallmentRow>(
        {ui::TableColumn{.label = "#", .width = ui::Sizing::fixed(4)},
         ui::TableColumn{.label = "PRINCIPAL", .width = ui::Sizing::stretch()},
         ui::TableColumn{.label = "INTEREST", .width = ui::Sizing::stretch()},
         ui::TableColumn{.label = "REMAINING", .width = ui::Sizing::stretch()}},
        [ctl] { return ctl->schedule(); },
        [](InstallmentRow const& row) { return ui::Key{static_cast<std::int64_t>(row.month)}; },
        [](morph::reactive::Signal<InstallmentRow> const& installment) {
            auto const* const row = &installment;
            return std::vector<ui::Node>{
                ui::text({.text = [row] { return row->get().monthText; }}),
                ui::text({.text = [row] { return row->get().principalText; }}),
                ui::text({.text = [row] { return row->get().interestText; }}),
                ui::text({.text = [row] { return row->get().remainingText; }}),
            };
        });

    return ui::column({
        .children = {applier,
                     ui::scroll({.child = list, .common = {.layout = {.height = ui::Sizing::fixed(10)}}}),
                     ui::panel({.title = "Amortization schedule",
                                .padding = 1,
                                .child = schedule,
                                .common = {.layout = {.height = ui::Sizing::stretch()}}})},
        .gap = 1,
    });
}

}  // namespace bank::client::views
```

- [ ] **Step 4: Generate the golden, then run the tests to verify they pass**

```bash
cmake --build build/bank --target bank_app_tests
BANK_UPDATE_GOLDEN=1 ./build/bank/examples/bank/bank_app_tests "Loans view: applying, showing the schedule and repaying through the screen"
cat examples/bank/tests/app/golden/loans.txt
```

Review it: a Panel `Apply for a loan` holding a Select with `options=[] selected=none`, TextInputs `Principal`,
`Rate (bps)`, `Months` and a Button `Apply` (`enabled=false`); a `Scroll` holding the list, an empty `Column`; a
Panel `Amortization schedule` with a `Table` whose columns are `#`, `PRINCIPAL`, `INTEREST`, `REMAINING` and no
`Row`s.

Run: `./build/bank/examples/bank/bank_app_tests "[bank][app]"`
Expected: PASS — 46 test cases.

Mutation checks, each restored afterwards:
1. In `_canApply`, replace `_rateBps.get().value_or(-1) >= 0` with `true`. Expected FAIL in "rate and term must be
   whole numbers; a zero rate is allowed" at the first `CHECK_FALSE`.
2. In `repay`, pay `row.outstanding / 2`. Expected FAIL in "repaying the outstanding balance pays the loan off"
   (times out waiting for `Paid off`).

- [ ] **Step 5: Commit**

```bash
git add examples/bank/app examples/bank/tests/app examples/bank/CMakeLists.txt
git commit -m "wip(bank): LoansController and the loans screen

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 8: `BankApplication`, the shell, the demo seed and `makeApplication`

The old `Main.qml` + `AppShell.qml` + the WASM main's `seedDemo()`. `BankApplication` is the `ui::Application`: it
owns the connection, the handlers, the notice strip and the six controllers (in that order, so the controllers die
first and the bridge last), and the route. Its view is a `Switch` over `screen()` — login or shell — above the
notice strip; the shell is a `Menu` and a `Switch` over the route. Opening a page refreshes it, as `AppShell.qml`'s
`refreshCurrent()` did. `--seed` (and the browser build, Task 10) signs in as `demo`, creating the user with two
EUR accounts and an opening deposit the first time — through the bridge, so it works the same over SQLite, over
the in-memory models and against `ladder_bank_server`.

**Files:**
- Create: `examples/bank/app/demo_seed.hpp`, `examples/bank/app/demo_seed.cpp`
- Create: `examples/bank/app/bank_application.hpp`, `examples/bank/app/bank_application.cpp`
- Create: `examples/bank/app/views/shell_view.hpp`, `examples/bank/app/views/shell_view.cpp`
- Create: `examples/bank/tests/app/golden/shell.txt` (generated in Step 4)
- Modify: `examples/bank/CMakeLists.txt` — append `app/demo_seed.cpp`, `app/bank_application.cpp` and
  `app/views/shell_view.cpp` to `BANK_APP_SOURCES`; append `tests/app/test_bank_application.cpp` to `bank_app_tests`
- Test: `examples/bank/tests/app/test_bank_application.cpp`

**Interfaces:**
- Consumes: Tasks 1–7; `ui::{Application, AppContext, switchOn, menu, Menu, MenuItem}` (Part 2);
  `reactive::Effect` (Part 1); `examples::{AppEnvironment::seed, connect, Connection{bridge, callbacks, ready},
  Link::Remote}` (Part 6); `backend::LocalBackend`, `IBackend::setConnectHandler` (`include/morph/core/backend.hpp`);
  `bank::{AccountKind, Currency}`.
- Produces:
  - `enum class Route : std::uint8_t { Accounts, MoveMoney, Cards, Payees, Loans }`, `routeTitle(Route)`;
    `enum class Screen : std::uint8_t { SignIn, Banking }`; `enum class DemoData : std::uint8_t { None, Seed }`.
  - `class DemoSeed { kUsername, kPassword, kDisplayName; DemoSeed(bridge::Bridge&, BankHandlers&, AuthController&,
    Notices&); finished(); }`.
  - `class BankApplication final : public ui::Application { BankApplication(ui::AppContext&,
    std::unique_ptr<examples::Connection>, DemoData = DemoData::None); view(); navigate(Route); signOut(); route();
    title(); screen(); handlers(); notices(); auth(); accounts(); transactions(); cards(); payees(); loans(); }` —
    the demo seed starts once `Connection::ready()` holds, at once for a local link.
  - `bank::client::makeApplication(ui::AppContext&, examples::AppEnvironment const&) ->
    std::unique_ptr<ui::Application>` — the contract's shape for every example (its closing "Conventions"), what
    `ui/main.cpp` (Task 9) hands the frontend. Everything else is `env` or a default: `env.db` empty is the
    temp-directory database, `env.seed` asks for the demo data, and a WebAssembly build always seeds, because the
    hosted page has no command line.
  - `views::noticeView(Notices const&)`, `views::shellView(BankApplication&)`, `views::appView(BankApplication&)`.

- [ ] **Step 1: Write the failing tests**

Create `examples/bank/tests/app/test_bank_application.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <app/transport.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <functional>
#include <memory>
#include <morph/core/backend.hpp>
#include <morph/core/executor.hpp>
#include <string>
#include <testkit/wait.hpp>
#include <utility>
#include <vector>

#include "bank/dto/account_dto.hpp"
#include "bank_app_test_support.hpp"
#include "bank_application.hpp"
#include "local_setup.hpp"
#include "recorded.hpp"

using bank::client::BankApplication;
using bank::client::DemoData;
using bank::client::Route;
using bank::client::Screen;
using bank::testing::FakeAppContext;
using bank::testing::RecordingBackend;
using bank::testing::settle;

namespace {

struct AppWiring {
    explicit AppWiring(DemoData demo = DemoData::None)
        : app{std::make_unique<BankApplication>(
              ctx,
              morph::examples::connect(ctx, bank::testing::localEnvironment(), bank::client::localSetup()),
              demo)} {}

    FakeAppContext ctx;
    std::unique_ptr<BankApplication> app;
};

// The local models behind a link that reports itself up the way a remote transport does: only when the test
// calls the connect hook it was handed.
class LateBackend final : public morph::backend::LocalBackend {
public:
    LateBackend(morph::exec::IExecutor& pool, std::function<void()>& connected)
        : LocalBackend{pool}, _connected{&connected} {}

    void setConnectHandler(std::function<void()> const& handler) override {
        if (handler) {
            *_connected = handler;
        }
    }

private:
    std::function<void()>* _connected;
};

}  // namespace

TEST_CASE("BankApplication: signed out it shows the login screen; signing in shows the shell on Accounts",
          "[bank][app][application]") {
    AppWiring wiring;
    RecordingBackend rec;
    auto const mounted = bank::testing::mount(wiring.ctx, rec, wiring.app->view());
    CHECK(wiring.app->screen() == Screen::SignIn);
    static_cast<void>(bank::testing::button(rec, "Sign in"));

    static_cast<void>(bank::testing::signUp(wiring.ctx, wiring.app->auth(), "app-shell"));
    bank::testing::drain(wiring.ctx);
    CHECK(wiring.app->screen() == Screen::Banking);
    CHECK(wiring.app->route() == Route::Accounts);
    CHECK(bank::testing::shows(rec, "Accounts"));
    CHECK_FALSE(rec.find(bank::testing::kButton, "label", "Sign in").has_value());
}

TEST_CASE("BankApplication: navigating refreshes the page it opens", "[bank][app][application]") {
    AppWiring wiring;
    static_cast<void>(bank::testing::signUp(wiring.ctx, wiring.app->auth(), "app-nav"));
    REQUIRE(settle(wiring.ctx, [&wiring] { return wiring.app->accounts().loaded(); }));
    wiring.app->navigate(Route::MoveMoney);
    CHECK(wiring.app->title() == "Move Money");

    // Opened behind the accounts page's back; only the navigation's refresh can show it.
    static_cast<void>(
        bank::testing::awaitReply(wiring.ctx, wiring.app->handlers().customers.execute(bank::dto::OpenAccount{})));
    bank::testing::drain(wiring.ctx);
    CHECK(wiring.app->accounts().cards().empty());
    wiring.app->navigate(Route::Accounts);
    REQUIRE(settle(wiring.ctx, [&wiring] { return wiring.app->accounts().cards().size() == 1; }));
    CHECK(wiring.app->title() == "Accounts");
}

TEST_CASE("BankApplication: logging out returns to the login screen on Accounts", "[bank][app][application]") {
    AppWiring wiring;
    static_cast<void>(bank::testing::signUp(wiring.ctx, wiring.app->auth(), "app-logout"));
    wiring.app->navigate(Route::Cards);
    wiring.app->signOut();
    CHECK(wiring.app->screen() == Screen::SignIn);
    CHECK(wiring.app->route() == Route::Accounts);
    CHECK_FALSE(wiring.app->auth().signedIn());
}

TEST_CASE("BankApplication: the demo seed signs in as demo with two accounts, and a second start adds none",
          "[bank][app][application][seed]") {
    {
        AppWiring wiring{DemoData::Seed};
        REQUIRE(settle(wiring.ctx, [&wiring] { return wiring.app->auth().signedIn(); }));
        CHECK(wiring.app->auth().principal() == "demo");
        CHECK(wiring.app->auth().displayName() == "Demo User");
        REQUIRE(settle(wiring.ctx, [&wiring] { return wiring.app->accounts().cards().size() == 2; }));
        CHECK(wiring.app->accounts().totalText() == "4800.00 EUR");
        CHECK(wiring.app->accounts().cards().front().overdraftText == "Overdraft 500.00 EUR");
    }
    {
        AppWiring wiring{DemoData::Seed};
        REQUIRE(settle(wiring.ctx,
                       [&wiring] { return wiring.app->auth().signedIn() && wiring.app->accounts().loaded(); }));
        CHECK(wiring.app->accounts().cards().size() == 2);
    }
}

TEST_CASE("BankApplication: the demo seed waits until a remote transport is up", "[bank][app][application][seed]") {
    FakeAppContext ctx;
    bank::client::localSetup().setupDatabase(bank::testing::localEnvironment().db);
    std::function<void()> connected;
    auto pool = std::make_unique<morph::exec::ThreadPoolExecutor>(2);
    auto backend = std::make_unique<LateBackend>(*pool, connected);
    auto const app = std::make_unique<BankApplication>(
        ctx,
        std::make_unique<morph::examples::Connection>(ctx, morph::examples::Link::Remote, std::move(backend),
                                                      std::move(pool)),
        DemoData::Seed);

    // Nothing is sent while the link is down: a remote transport rejects calls issued before it connects.
    CHECK_FALSE(morph::examples::testing::pumpUntil(ctx.owner(), [&app] { return app->auth().signedIn(); },
                                                    std::chrono::milliseconds{500}));
    REQUIRE(connected);
    connected();  // the transport's thread; the Connection posts the change to the owner
    REQUIRE(settle(ctx, [&app] { return app->auth().signedIn(); }));
    CHECK(app->auth().principal() == "demo");
}

TEST_CASE("Shell view: the sidebar, the title and the page", "[bank][app][view][application]") {
    AppWiring wiring;
    std::string const principal = bank::testing::signUp(wiring.ctx, wiring.app->auth(), "view-shell");
    REQUIRE(settle(wiring.ctx, [&wiring] { return wiring.app->accounts().loaded(); }));
    RecordingBackend rec;
    auto const mounted = bank::testing::mount(wiring.ctx, rec, wiring.app->view());
    bank::testing::checkGolden("shell", rec.dump(), {{principal, "<user>"}});
    CHECK(bank::testing::shows(rec, "@" + principal));

    std::vector<int> const menus = rec.all(bank::testing::kMenu);
    REQUIRE(menus.size() == 1);
    rec.chooseIndex(menus.front(), 1);
    bank::testing::drain(wiring.ctx);
    CHECK(wiring.app->route() == Route::MoveMoney);
    CHECK(bank::testing::shows(rec, "Move Money"));

    rec.click(bank::testing::button(rec, "Log out"));
    bank::testing::drain(wiring.ctx);
    CHECK(wiring.app->screen() == Screen::SignIn);
    static_cast<void>(bank::testing::button(rec, "Sign in"));
}
```

- [ ] **Step 2: Run them to verify they fail**

Run: `cmake --build build/bank --target bank_app_tests`
Expected: FAIL — `fatal error: 'bank_application.hpp' file not found`.

- [ ] **Step 3: Implement**

Create `examples/bank/app/demo_seed.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <exception>
#include <morph/core/bridge.hpp>
#include <morph/core/callback_scope.hpp>
#include <string_view>

#include "bank/dto/auth_dto.hpp"
#include "bank_handlers.hpp"
#include "notices.hpp"

/// @file
/// The demo data the hosted page and `--seed` start with: user `demo` / `demo1234`, a checking account
/// with a 500.00 EUR overdraft and a 4,800.00 EUR opening deposit, and a savings account.

namespace bank::client {

class AuthController;

/// @brief Signs in as the demo user, creating it and its accounts first when it does not exist.
///
/// Everything goes through the bridge, as a user's own calls would, so it behaves the same over the
/// SQLite models, the in-memory models and a remote server. The session is installed before the
/// accounts are opened (they need a principal) and the user is signed in only once they exist, so
/// the screens' first fetch already lists them.
class DemoSeed {
public:
    static constexpr std::string_view kUsername = "demo";
    static constexpr std::string_view kPassword = "demo1234";
    static constexpr std::string_view kDisplayName = "Demo User";

    /// @brief Starts seeding at once. Every parameter is borrowed and must outlive this object.
    DemoSeed(morph::bridge::Bridge& bridge, BankHandlers& handlers, AuthController& auth, Notices& notices);

    /// @brief Whether the seed signed in or gave up.
    [[nodiscard]] bool finished() const noexcept { return _finished; }

private:
    void onSignIn(dto::AuthResult const& result);
    void onRegistered(dto::AuthResult const& result);
    void onChecking(dto::AuthResult const& user, std::int64_t checkingId);
    void onDeposited(dto::AuthResult const& user);
    void fail(std::exception_ptr const& error);

    morph::bridge::Bridge* _bridge;
    BankHandlers* _handlers;
    AuthController* _auth;
    Notices* _notices;
    bool _finished = false;
    // Last member, so it is destroyed first: a reply arriving after the application is gone is dropped.
    morph::async::CallbackScope _calls;
};

}  // namespace bank::client
```

Create `examples/bank/app/demo_seed.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include "demo_seed.hpp"

#include <morph/reactive/control.hpp>
#include <morph/session/session.hpp>
#include <string>
#include <utility>

#include "bank/core/types.hpp"
#include "bank/dto/account_dto.hpp"
#include "bank/dto/transaction_dto.hpp"
#include "controllers/auth_controller.hpp"

namespace bank::client {

DemoSeed::DemoSeed(morph::bridge::Bridge& bridge, BankHandlers& handlers, AuthController& auth, Notices& notices)
    : _bridge{&bridge}, _handlers{&handlers}, _auth{&auth}, _notices{&notices} {
    _handlers->auth
        .execute(dto::LoginRequest{.username = std::string{kUsername}, .password = std::string{kPassword}})
        .then(_calls, [this](dto::AuthResult const& result) { onSignIn(result); })
        .onError(_calls, [this](std::exception_ptr error) { fail(error); });
}

void DemoSeed::onSignIn(dto::AuthResult const& result) {
    if (result.ok) {
        _auth->adopt(result);
        _finished = true;
        return;
    }
    _handlers->auth
        .execute(dto::RegisterUser{.username = std::string{kUsername},
                                   .password = std::string{kPassword},
                                   .displayName = std::string{kDisplayName}})
        .then(_calls, [this](dto::AuthResult const& registered) { onRegistered(registered); })
        .onError(_calls, [this](std::exception_ptr error) { fail(error); });
}

void DemoSeed::onRegistered(dto::AuthResult const& result) {
    if (!result.ok) {
        _auth->adopt(result);  // shows the refusal
        _finished = true;
        return;
    }
    morph::session::Context session;
    session.principal = result.principal;
    _bridge->setDefaultSession(std::move(session));
    _handlers->customers
        .execute(dto::OpenAccount{.kind = static_cast<int>(bank::AccountKind::Checking),
                                  .currency = static_cast<int>(bank::Currency::EUR),
                                  .overdraftMinor = 50000})
        .then(_calls, [this, result](dto::AccountInfo const& checking) { onChecking(result, checking.id); })
        .onError(_calls, [this](std::exception_ptr error) { fail(error); });
}

void DemoSeed::onChecking(dto::AuthResult const& user, std::int64_t checkingId) {
    _handlers->transactions
        .execute(dto::Deposit{.accountId = checkingId, .amountMinor = 480000, .description = "opening deposit"})
        .then(_calls, [this, user](dto::TxnInfo const& /*deposit*/) { onDeposited(user); })
        .onError(_calls, [this](std::exception_ptr error) { fail(error); });
}

void DemoSeed::onDeposited(dto::AuthResult const& user) {
    _handlers->customers
        .execute(dto::OpenAccount{.kind = static_cast<int>(bank::AccountKind::Savings),
                                  .currency = static_cast<int>(bank::Currency::EUR)})
        .then(_calls,
              [this, user](dto::AccountInfo const& /*savings*/) {
                  _auth->adopt(user);
                  _finished = true;
              })
        .onError(_calls, [this](std::exception_ptr error) { fail(error); });
}

void DemoSeed::fail(std::exception_ptr const& error) {
    _notices->error(morph::reactive::errorMessage(error));
    _finished = true;
}

}  // namespace bank::client
```

Create `examples/bank/app/bank_application.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <app/app_environment.hpp>
#include <app/transport.hpp>
#include <cstdint>
#include <memory>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/signal.hpp>
#include <morph/ui/frontend.hpp>
#include <morph/ui/view.hpp>
#include <string>
#include <string_view>

#include "bank_handlers.hpp"
#include "controllers/accounts_controller.hpp"
#include "controllers/auth_controller.hpp"
#include "controllers/cards_controller.hpp"
#include "controllers/loans_controller.hpp"
#include "controllers/payees_controller.hpp"
#include "controllers/transactions_controller.hpp"
#include "demo_seed.hpp"
#include "notices.hpp"

/// @file
/// The bank client as one `ui::Application`, and the factory the binary hands its frontend.

namespace bank::client {

/// @brief The signed-in user's pages, in sidebar order.
enum class Route : std::uint8_t { Accounts, MoveMoney, Cards, Payees, Loans };

/// @brief Which top-level screen is shown.
enum class Screen : std::uint8_t { SignIn, Banking };

/// @brief Whether the application signs in as the demo user on start.
enum class DemoData : std::uint8_t { None, Seed };

/// @brief A page's heading, which is also its sidebar entry.
[[nodiscard]] std::string_view routeTitle(Route route) noexcept;

/// @brief The bank client: the session, six screens, the route between them and one notice strip.
class BankApplication final : public morph::ui::Application {
public:
    /// @param ctx The frontend's context: the runtime, and the scheduler the notice strip hides on.
    /// @param connection The bridge every handler calls through. Owned: it outlives the controllers.
    /// @param demo Whether to sign in as the demo user, as soon as the connection is ready.
    BankApplication(morph::ui::AppContext& ctx, std::unique_ptr<morph::examples::Connection> connection,
                    DemoData demo = DemoData::None);
    ~BankApplication() override = default;
    BankApplication(BankApplication const&) = delete;
    BankApplication& operator=(BankApplication const&) = delete;
    BankApplication(BankApplication&&) = delete;
    BankApplication& operator=(BankApplication&&) = delete;

    [[nodiscard]] morph::ui::Node view() override;

    /// @brief Opens @p route, fetching its page again; opening the page already shown does nothing.
    void navigate(Route route);
    /// @brief Signs out and returns to the first page for the next sign-in.
    void signOut();

    [[nodiscard]] Route route() const { return _route.get(); }
    [[nodiscard]] std::string const& title() const { return _title.get(); }
    [[nodiscard]] Screen screen() const { return _screen.get(); }

    [[nodiscard]] BankHandlers& handlers() noexcept { return _handlers; }
    [[nodiscard]] Notices& notices() noexcept { return _notices; }
    [[nodiscard]] AuthController& auth() noexcept { return _auth; }
    [[nodiscard]] AccountsController& accounts() noexcept { return _accounts; }
    [[nodiscard]] TransactionsController& transactions() noexcept { return _transactions; }
    [[nodiscard]] CardsController& cards() noexcept { return _cards; }
    [[nodiscard]] PayeesController& payees() noexcept { return _payees; }
    [[nodiscard]] LoansController& loans() noexcept { return _loans; }

private:
    morph::reactive::Runtime* _rt;
    std::unique_ptr<morph::examples::Connection> _connection;
    BankHandlers _handlers;
    Notices _notices;
    AuthController _auth;
    AccountsController _accounts;
    TransactionsController _transactions;
    CardsController _cards;
    PayeesController _payees;
    LoansController _loans;
    morph::reactive::Signal<Route> _route;
    morph::reactive::Computed<std::string> _title;
    morph::reactive::Computed<Screen> _screen;
    std::unique_ptr<DemoSeed> _seed;
    // Starts `_seed` once the connection is ready; after `_seed`, so it is destroyed first.
    std::unique_ptr<morph::reactive::Effect> _seedWhenReady;
};

/// @brief Connects as @p env says (local by default, `--server` for remote) and builds the application.
///
/// Every knob is `env` or a default: an empty `env.db` is `morph_bank.db` in the temp directory, `env.seed` signs
/// in as the demo user, and a WebAssembly build always does, because the hosted page has no command line.
/// @param ctx The frontend's context.
/// @param env The command line's (or, in the browser, the page's) settings.
/// @return The application.
/// @throws morph::examples::TransportError when `--server` is given and this frontend cannot reach one.
[[nodiscard]] std::unique_ptr<morph::ui::Application> makeApplication(morph::ui::AppContext& ctx,
                                                                      morph::examples::AppEnvironment const& env);

}  // namespace bank::client
```

Create `examples/bank/app/bank_application.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include "bank_application.hpp"

#include <utility>

#include "local_setup.hpp"
#include "views/shell_view.hpp"

namespace bank::client {

std::string_view routeTitle(Route route) noexcept {
    switch (route) {
        case Route::Accounts:
            return "Accounts";
        case Route::MoveMoney:
            return "Move Money";
        case Route::Cards:
            return "Cards";
        case Route::Payees:
            return "Payees & Bills";
        case Route::Loans:
            return "Loans";
    }
    return "Accounts";
}

BankApplication::BankApplication(morph::ui::AppContext& ctx, std::unique_ptr<morph::examples::Connection> connection,
                                 DemoData demo)
    : _rt{&ctx.runtime()},
      _connection{std::move(connection)},
      _handlers{_connection->bridge(), _connection->callbacks()},
      _notices{ctx.runtime(), ctx.scheduler()},
      _auth{ctx.runtime(), _connection->bridge(), _handlers, _notices},
      _accounts{ctx.runtime(), _handlers, _auth, _notices},
      _transactions{ctx.runtime(), _handlers, _auth, _notices},
      _cards{ctx.runtime(), _handlers, _auth, _notices},
      _payees{ctx.runtime(), _handlers, _auth, _notices},
      _loans{ctx.runtime(), _handlers, _auth, _notices},
      _route{ctx.runtime(), Route::Accounts},
      _title{ctx.runtime(), [this] { return std::string{routeTitle(_route.get())}; }},
      _screen{ctx.runtime(), [this] { return _auth.signedIn() ? Screen::Banking : Screen::SignIn; }} {
    if (demo == DemoData::Seed) {
        // A remote transport rejects calls issued before it connects; a local link is ready at once, so this
        // effect's first run starts the seed there.
        _seedWhenReady = std::make_unique<morph::reactive::Effect>(ctx.runtime(), [this] {
            if (_seed == nullptr && _connection->ready().get()) {
                _seed = std::make_unique<DemoSeed>(_connection->bridge(), _handlers, _auth, _notices);
            }
        });
    }
}

morph::ui::Node BankApplication::view() { return views::appView(*this); }

void BankApplication::navigate(Route route) {
    if (_route.peek() == route) {
        return;
    }
    _rt->batch([&] {
        _route.set(route);
        switch (route) {
            case Route::Accounts:
                _accounts.refresh();
                break;
            case Route::MoveMoney:
                _transactions.refresh();
                break;
            case Route::Cards:
                _cards.refresh();
                break;
            case Route::Payees:
                _payees.refresh();
                break;
            case Route::Loans:
                _loans.refresh();
                break;
        }
    });
}

void BankApplication::signOut() {
    _rt->batch([this] {
        _auth.signOut();
        _route.set(Route::Accounts);
    });
}

std::unique_ptr<morph::ui::Application> makeApplication(morph::ui::AppContext& ctx,
                                                        morph::examples::AppEnvironment const& env) {
#if defined(__EMSCRIPTEN__)
    constexpr bool kAlwaysSeed = true;
#else
    constexpr bool kAlwaysSeed = false;
#endif
    return std::make_unique<BankApplication>(ctx, morph::examples::connect(ctx, env, localSetup()),
                                             env.seed || kAlwaysSeed ? DemoData::Seed : DemoData::None);
}

}  // namespace bank::client
```

Create `examples/bank/app/views/shell_view.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/ui/view.hpp>

namespace bank::client {
class BankApplication;
class Notices;
}  // namespace bank::client

namespace bank::client::views {

/// @brief The notice strip: the last notice, shown while it is visible, in its tone.
[[nodiscard]] morph::ui::Node noticeView(Notices const& notices);

/// @brief The signed-in shell: the sidebar menu, the page heading and the current page.
[[nodiscard]] morph::ui::Node shellView(BankApplication& app);

/// @brief The whole client: login or the shell, above the notice strip.
[[nodiscard]] morph::ui::Node appView(BankApplication& app);

}  // namespace bank::client::views
```

Create `examples/bank/app/views/shell_view.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include "views/shell_view.hpp"

#include "bank_application.hpp"
#include "views/accounts_view.hpp"
#include "views/cards_view.hpp"
#include "views/login_view.hpp"
#include "views/loans_view.hpp"
#include "views/move_money_view.hpp"
#include "views/payees_view.hpp"
#include "views/style.hpp"

namespace bank::client::views {

namespace ui = morph::ui;

ui::Node noticeView(Notices const& notices) {
    Notices const* const strip = &notices;
    return ui::text({.text = [strip] { return strip->text(); },
                     .role = [strip] { return roleOf(strip->tone()); },
                     .common = {.visible = [strip] { return strip->visible(); }}});
}

ui::Node shellView(BankApplication& app) {
    BankApplication* const shell = &app;
    AuthController const* const auth = &app.auth();
    auto const navigateTo = [shell](Route route) { return [shell, route] { shell->navigate(route); }; };

    ui::Node sidebar = ui::column({
        .children = {ui::text({.text = "Morph Bank", .role = ui::TextRole::Heading}),
                     ui::text({.text = "personal banking", .role = ui::TextRole::Muted}),
                     ui::menu({.items = {ui::MenuItem{.label = "Accounts", .onSelect = navigateTo(Route::Accounts)},
                                         ui::MenuItem{.label = "Move Money", .onSelect = navigateTo(Route::MoveMoney)},
                                         ui::MenuItem{.label = "Cards", .onSelect = navigateTo(Route::Cards)},
                                         ui::MenuItem{.label = "Payees & Bills", .onSelect = navigateTo(Route::Payees)},
                                         ui::MenuItem{.label = "Loans", .onSelect = navigateTo(Route::Loans)}}}),
                     ui::spacer({.common = {.layout = {.height = ui::Sizing::stretch()}}}),
                     ui::text({.text = [auth] { return auth->displayName(); }}),
                     ui::text({.text = [auth] { return auth->handle(); }, .role = ui::TextRole::Muted}),
                     ui::button({.label = "Log out", .onClick = [shell] { shell->signOut(); }})},
        .gap = 1,
        .common = {.layout = {.width = ui::Sizing::fixed(24), .height = ui::Sizing::stretch()}},
    });

    ui::Node page = ui::switchOn<Route>([shell] { return shell->route(); },
                                        {{Route::Accounts, accountsView(app.accounts())},
                                         {Route::MoveMoney, moveMoneyView(app.transactions())},
                                         {Route::Cards, cardsView(app.cards())},
                                         {Route::Payees, payeesView(app.payees())},
                                         {Route::Loans, loansView(app.loans())}});

    ui::Node content = ui::column({
        .children = {ui::text({.text = [shell] { return shell->title(); }, .role = ui::TextRole::Heading}), page},
        .gap = 1,
        .common = {.layout = {.width = ui::Sizing::stretch(), .height = ui::Sizing::stretch()}},
    });

    return ui::row({.children = {sidebar, content}, .gap = 2});
}

ui::Node appView(BankApplication& app) {
    BankApplication* const shell = &app;
    return ui::column({
        .children = {ui::switchOn<Screen>([shell] { return shell->screen(); },
                                          {{Screen::SignIn, loginView(app.auth())}, {Screen::Banking, shellView(app)}}),
                     noticeView(app.notices())},
        .gap = 1,
    });
}

}  // namespace bank::client::views
```

- [ ] **Step 4: Generate the golden, then run the tests to verify they pass**

```bash
cmake --build build/bank --target bank_app_tests
BANK_UPDATE_GOLDEN=1 ./build/bank/examples/bank/bank_app_tests "Shell view: the sidebar, the title and the page"
cat examples/bank/tests/app/golden/shell.txt
```

Review it: a `Column` holding the screen switch (a `Slot`) and a Text with `visible=false` (the notice strip);
inside the `Slot`, a `Row` of the sidebar — a `Column` with Heading `Morph Bank`, Muted `personal banking`, a `Menu`
with `items=[Accounts,Move Money,Cards,Payees & Bills,Loans]`, a `Spacer`, Text `Tester`, Muted `@<user>`, Button
`Log out` — and the content `Column` with Heading `Accounts` above the page's `Slot`, holding the accounts page as
`accounts.txt` shows it (total `0`). No login form.

Run: `./build/bank/examples/bank/bank_app_tests "[bank][app]"`
Expected: PASS — 52 test cases.

Mutation checks, each restored afterwards:
1. In `BankApplication::navigate`, delete the `switch`. Expected FAIL in "navigating refreshes the page it opens"
   (times out with no cards).
2. In `DemoSeed::onDeposited`, delete `_auth->adopt(user);`. Expected FAIL in "the demo seed signs in as demo with
   two accounts, and a second start adds none" (times out waiting for `signedIn()`).
3. In `BankApplication::signOut`, delete `_route.set(Route::Accounts);`. Expected FAIL in "logging out returns to the
   login screen on Accounts".
4. In `BankApplication`'s constructor, drop `&& _connection->ready().get()` from the seed effect. Expected FAIL in
   "the demo seed waits until a remote transport is up" at the `CHECK_FALSE` (it signs in before the link is up).

- [ ] **Step 5: Commit**

```bash
git add examples/bank/app examples/bank/tests/app examples/bank/CMakeLists.txt
git commit -m "wip(bank): BankApplication, the shell, the demo seed and makeApplication

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 9: The `bank` binary and its frontend smoke tests

`ui/main.cpp` is spec 4 §3's composition root, unchanged: it reads the environment, offers every frontend this
configure built (Qt Quick first, then the terminal), lets `ui::selectFrontend` choose (`--ui=`, `MORPH_UI`, else the
first usable) and runs `bank::client::makeApplication(ctx, env)`. Everything bank-specific — the default database,
the browser's demo seed — lives in `makeApplication` (Task 8). The binary is built when at least one frontend is;
`bank_app` and its tests are not gated on either (spec 4 §6). The smoke tests are a binary of their own,
`bank_smoke_tests`, because the Qt Quick frontend constructs its own `QGuiApplication` and so runs only under a
`main` that owns no Qt application object (`morph_test_main`).

**Files:**
- Create: `examples/bank/ui/main.cpp`
- Modify: `examples/bank/CMakeLists.txt` — (a) after the `bank_app` block (ending with the toolkit-free
  `foreach`), the `bank` executable; (b) in the tests block, after `catch_discover_tests(bank_app_tests …)`, the
  `bank_smoke_tests` executable
- Test: `examples/bank/tests/smoke/test_bank_frontend_smoke.cpp`

**Interfaces:**
- Consumes: Task 8's `bank::client::makeApplication`; `ui::{FrontendOption, selectFrontend, AppContext,
  ApplicationFactory, FrontendSelectionError}` (Part 2, `morph/ui/frontend.hpp`); `tui::frontendOption(FrontendConfig
  = {})` (Part 3, `morph/tui/frontend.hpp`); `qt_quick::frontendOption(int&, char**,
  ui::EnvironmentReader = ui::processEnvironment())` (Part 4, `morph/qt_quick/frontend.hpp`); `examples::AppEnvironment::fromArgs` (Part 6); `examples::testing::{runFrontendSmoke,
  SmokeFrontend}` (`testkit/frontend_smoke.hpp`, target `morph::example_testkit`) and CMake
  `morph_example_frontends(<target> <scope>)` (`cmake/morph_example_app.cmake`) (Part 6).
- Produces: CMake target `bank` (native), target `bank_smoke_tests`.

- [ ] **Step 1: Write the failing test**

Create `examples/bank/tests/smoke/test_bank_frontend_smoke.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// The application mounts and quits on every frontend this configure built: makeApplication over this
// process's own local database, on the terminal through scripted input and on Qt Quick offscreen. A case for
// a frontend the configure did not build is skipped by the harness.

#include <app/app_environment.hpp>
#include <catch2/catch_test_macros.hpp>
#include <morph/ui/frontend.hpp>
#include <testkit/frontend_smoke.hpp>

#include "bank_application.hpp"
#include "bank_test_support.hpp"

namespace {

morph::ui::ApplicationFactory bankFactory() {
    return [](morph::ui::AppContext& ctx) {
        morph::examples::AppEnvironment env;
        env.db = bank::testing::connectionString();
        return bank::client::makeApplication(ctx, env);
    };
}

}  // namespace

TEST_CASE("bank mounts and quits on the terminal frontend", "[bank][smoke][tui]") {
    morph::examples::testing::runFrontendSmoke(bankFactory(), morph::examples::testing::SmokeFrontend::Tui);
}

TEST_CASE("bank mounts and quits on Qt Quick, offscreen", "[bank][smoke][qt]") {
    morph::examples::testing::runFrontendSmoke(bankFactory(), morph::examples::testing::SmokeFrontend::QtQuick);
}
```

In `examples/bank/CMakeLists.txt`, place (b) — after `catch_discover_tests(bank_app_tests …)`:

```cmake
        # ── bank_smoke_tests: the application on each built frontend ─────────
        # Mounts makeApplication's view and quits -- on the terminal through
        # scripted input, on Qt Quick offscreen. A binary of its own because
        # the Qt Quick frontend constructs its own QGuiApplication, and
        # morph_test_main owns no Qt application object. The harness is
        # morph::example_testkit (examples/common/app), which links every built
        # frontend; it is linked by name, which CMake resolves at generate time.
        # It exists only where the bank binary does.
        if(TARGET bank)
            add_executable(bank_smoke_tests tests/smoke/test_bank_frontend_smoke.cpp)
            target_include_directories(bank_smoke_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
            target_link_libraries(bank_smoke_tests PRIVATE bank_app morph::example_testkit morph_test_main)
            target_compile_features(bank_smoke_tests PRIVATE cxx_std_23)
            apply_bigobj(bank_smoke_tests)
            if(DEFINED AF_SANITIZER)
                apply_sanitizers(bank_smoke_tests ${AF_SANITIZER})
            endif()
            # Offscreen: the runner may have no display, and this suite never
            # looks at a pixel. The QGuiApplication is created inside the test
            # case, not in main, so discovery needs no platform plugin.
            catch_discover_tests(bank_smoke_tests
                DISCOVERY_MODE PRE_TEST
                PROPERTIES
                    LABELS "bank"
                    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
        endif()
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cmake -S . -B build/bank && cmake --build build/bank --target bank_smoke_tests`
Expected: FAIL — `ninja: error: unknown target 'bank_smoke_tests'` (no `bank` target yet, so the block is skipped).

- [ ] **Step 3: Implement**

Create `examples/bank/ui/main.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// The bank client's composition root: read the environment, offer every frontend this build has,
// let the user or the environment pick one, and run the application on it. The same file is the
// browser build's main, with Qt Quick the only frontend.

#include <app/app_environment.hpp>
#include <exception>
#include <iostream>
#include <morph/ui/frontend.hpp>
#include <vector>

#include "bank_application.hpp"

#if MORPH_EXAMPLE_HAS_QT_QUICK
#include <morph/qt_quick/frontend.hpp>
#endif
#if MORPH_EXAMPLE_HAS_TUI
#include <morph/tui/frontend.hpp>
#endif

int main(int argc, char** argv) {
    try {
        auto const env = morph::examples::AppEnvironment::fromArgs(argc, argv);
        std::vector<morph::ui::FrontendOption> built;
#if MORPH_EXAMPLE_HAS_QT_QUICK
        built.push_back(morph::qt_quick::frontendOption(argc, argv));
#endif
#if MORPH_EXAMPLE_HAS_TUI
        built.push_back(morph::tui::frontendOption());
#endif
        auto const frontend = morph::ui::selectFrontend(built, argc, argv);
        return frontend->run([&env](morph::ui::AppContext& ctx) { return bank::client::makeApplication(ctx, env); });
    } catch (std::exception const& error) {
        std::cerr << "bank: " << error.what() << '\n';
    } catch (...) {
        std::cerr << "bank: unknown error\n";
    }
    return 1;
}
```

In `examples/bank/CMakeLists.txt`, place (a) — after the `bank_app` block:

```cmake
# ── bank: the one client binary ──────────────────────────────────────────────
# ui/main.cpp links every frontend this configure built and picks one at run
# time (--ui=, MORPH_UI, else the first usable). Built when at least one
# frontend is. morph_example_frontends (cmake/morph_example_app.cmake) links
# them and defines MORPH_EXAMPLE_HAS_TUI / MORPH_EXAMPLE_HAS_QT_QUICK for
# main's #if; it names the frontend targets, which the root creates after it
# adds examples/bank and CMake resolves at generate time. Not
# morph_add_example_ui, which calls apply_warnings(): bank_app's headers bring
# the ORM's, which are not -Werror clean (bank_lib's reason).
if(MORPH_BUILD_TUI OR MORPH_BUILD_QT_QUICK)
    if(MORPH_BUILD_QT_QUICK)
        find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2)
        qt_add_executable(bank ui/main.cpp)
    else()
        add_executable(bank ui/main.cpp)
    endif()
    target_link_libraries(bank PRIVATE bank_app)
    morph_example_frontends(bank PRIVATE)
    target_compile_features(bank PRIVATE cxx_std_23)
    apply_bigobj(bank)
    if(DEFINED AF_SANITIZER)
        apply_sanitizers(bank ${AF_SANITIZER})
    endif()
else()
    message(STATUS "examples/bank: the bank binary needs MORPH_BUILD_TUI or MORPH_BUILD_QT_QUICK; not built")
endif()
```

- [ ] **Step 4: Run the tests to verify they pass, and run the binary**

```bash
cmake --build build/bank --target bank bank_smoke_tests
ctest --test-dir build/bank -L bank -R 'bank mounts' --output-on-failure
```

Expected: PASS — two tests (`… on the terminal frontend`, `… on Qt Quick, offscreen`), neither skipped on
`build/bank`, which builds both frontends.

Then by hand, each from a terminal (the TUI needs one):

```bash
./build/bank/examples/bank/bank --ui=tui --seed     # opens signed in as Demo User; Tab to the menu,
                                                    # Enter on "Move Money", deposit 10, Ctrl+C quits
./build/bank/examples/bank/bank --ui=qt --seed      # the same in a window
./build/bank/examples/bank/bank --ui=nonsense; echo "exit $?"
                                                    # "bank: …" naming the built frontends, exit 1
cmake --build build/bank --target ladder_bank_server
BANK_DB="DRIVER=SQLite3;Database=$PWD/build/bank/remote.db;Timeout=5000" BANK_PORT=54321 \
    ./build/bank/examples/bank/ladder_bank_server &
./build/bank/examples/bank/bank --ui=qt --server ws://127.0.0.1:54321 --seed
./build/bank/examples/bank/bank --ui=tui --server ws://127.0.0.1:54321    # sign in as demo / demo1234
kill %1
```

Expected: both frontends show the demo user's two accounts; the remote runs reach the server (its log shows the
calls) and a deposit in one client shows in the other after navigating. Record what was seen for Task 13's
commit body — these runs are observed, not asserted by a test.

Mutation check: make `BankApplication::view()` throw `std::runtime_error{"broken"}`. Expected FAIL in both smoke
tests (the run does not exit 0). Restore.

- [ ] **Step 5: Commit**

```bash
git add examples/bank/ui examples/bank/tests/smoke examples/bank/CMakeLists.txt
git commit -m "wip(bank): the bank binary on an injected frontend, and its smoke tests

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 10: The browser build on the in-memory models

The WebAssembly client becomes the same `ui/main.cpp` and app sources, compiled with Qt Quick as the only frontend
and against the in-memory models `gui_wasm/` already has: its shadow model headers, `bank::wasm` store and
`*_model_wasm.cpp` move to `examples/bank/wasm/` unchanged, and only the build around them is new. A native test
binary compiles the very same set, so a controller that calls an action the in-memory models do not serve fails a
local `ctest`, not only a browser.

**Files:**
- Move: `examples/bank/gui_wasm/` → `examples/bank/wasm/` (`git mv`); delete `examples/bank/wasm/main_wasm.cpp`
- Create: `examples/bank/wasm/local_setup_wasm.cpp`; rewrite `examples/bank/wasm/CMakeLists.txt`
- Modify: `examples/bank/wasm/include/bank/models/auth_model.hpp` (line 5) and
  `examples/bank/wasm/include/bank/wasm/store_ops.hpp` (lines 19–20) — comments that name the QML controllers
- Modify: `examples/bank/CMakeLists.txt` — (a) after `set(BANK_APP_SOURCES …)`, `set(BANK_INMEMORY_SOURCES …)`;
  (b) the `if(EMSCRIPTEN)` branch; (c) in the tests block, after the `bank_smoke_tests` block, `bank_inmemory_tests`
- Modify: `.github/workflows/wasm-demo.yml` — header comment, `paths`, both Qt installs, Configure, Build, Stage bundle
- Test: `examples/bank/tests/inmemory/test_inmemory_models.cpp`

**Interfaces:**
- Consumes: Tasks 1–9 (`BANK_APP_SOURCES`, `BankApplication`, `DemoData::Seed`, `localSetup()`'s declaration,
  `makeApplication`'s WebAssembly seed default); `examples::LocalSetup::workers`, `FakeAppContext` and CMake
  `morph_example_frontends` (Part 6); `morph::qt_quick` under Emscripten (Part 4, spec 3 §6).
- Produces: `bank::client::localSetup()` for the in-memory build (one worker, no database); CMake variable
  `BANK_INMEMORY_SOURCES`; the browser target `bank` (Emscripten); target `bank_inmemory_tests` (native).

- [ ] **Step 1: Write the failing test**

Create `examples/bank/tests/inmemory/test_inmemory_models.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// The browser build's model set, run natively: the app sources compiled against wasm/'s shadow model
// headers and in-memory models, started the way the hosted page starts — seeded and signed in — and
// then driven through a deposit. A controller calling an action the in-memory models do not serve
// fails here instead of only in a browser.

#include <app/app_environment.hpp>
#include <app/transport.hpp>
#include <catch2/catch_test_macros.hpp>
#include <memory>

#include "bank_application.hpp"
#include "local_setup.hpp"
#include "test_app_context.hpp"

using bank::client::BankApplication;
using bank::client::DemoData;
using bank::testing::settle;
using bank::testing::FakeAppContext;

TEST_CASE("the in-memory models serve the seeded demo and a deposit", "[bank][inmemory]") {
    FakeAppContext ctx;
    auto const app = std::make_unique<BankApplication>(
        ctx, morph::examples::connect(ctx, morph::examples::AppEnvironment{}, bank::client::localSetup()),
        DemoData::Seed);
    REQUIRE(settle(ctx, [&app] { return app->auth().signedIn(); }));
    CHECK(app->auth().principal() == "demo");
    REQUIRE(settle(ctx, [&app] { return app->accounts().cards().size() == 2; }));
    CHECK(app->accounts().totalText() == "4800.00 EUR");

    bank::client::TransactionsController& txns = app->transactions();
    REQUIRE(settle(ctx, [&txns] { return txns.accountChoices().size() == 2; }));
    txns.editAmount("200.00");
    txns.deposit();
    REQUIRE(settle(ctx, [&app] { return app->notices().visible(); }));
    CHECK(app->notices().text() == "Transaction posted");
    app->accounts().refresh();
    REQUIRE(settle(ctx, [&app] { return app->accounts().totalText() == "5000.00 EUR"; }));
}
```

In `examples/bank/CMakeLists.txt`, place (c) — after the `bank_smoke_tests` block:

```cmake
        # ── bank_inmemory_tests: the browser build's models, natively ────────
        # The app sources compiled exactly as wasm/ compiles them — shadow model
        # headers first, in-memory models, no bank_lib — so a controller that
        # calls an action the in-memory models do not serve fails here. One
        # worker (local_setup_wasm.cpp): the in-memory store has no locks,
        # because the browser runs everything on one thread.
        add_executable(bank_inmemory_tests
            tests/inmemory/test_inmemory_models.cpp
            ${BANK_APP_SOURCES}
            ${BANK_INMEMORY_SOURCES}
        )
        target_include_directories(bank_inmemory_tests BEFORE PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/wasm/include
            ${CMAKE_CURRENT_SOURCE_DIR}/app
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/app)
        target_link_libraries(bank_inmemory_tests PRIVATE morph::morph morph::ladder_app_common morph_test_main)
        target_compile_features(bank_inmemory_tests PRIVATE cxx_std_23)
        apply_bigobj(bank_inmemory_tests)
        if(DEFINED AF_SANITIZER)
            apply_sanitizers(bank_inmemory_tests ${AF_SANITIZER})
        endif()
        catch_discover_tests(bank_inmemory_tests
            DISCOVERY_MODE PRE_TEST
            PROPERTIES LABELS "bank")
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cmake -S . -B build/bank && cmake --build build/bank --target bank_inmemory_tests`
Expected: FAIL — `fatal error: 'Lightweight/Lightweight.hpp' file not found`: `wasm/include` does not exist yet, so
the model headers resolve to the native ones, which need the ORM this target does not link.

- [ ] **Step 3: Implement**

Move the in-memory models and retire the old entry point:

```bash
git mv examples/bank/gui_wasm examples/bank/wasm
git rm examples/bank/wasm/main_wasm.cpp
```

Edit the two comments that name the QML layer. In `examples/bank/wasm/include/bank/models/auth_model.hpp` replace

```cpp
// WASM shadow of include/bank/models/auth_model.hpp: the SAME class + action
// registrations the controllers/QML expect, but with no Lightweight/ODBC
```

with

```cpp
// WASM shadow of include/bank/models/auth_model.hpp: the SAME class + action
// registrations the app's controllers expect, but with no Lightweight/ODBC
```

and in `examples/bank/wasm/include/bank/wasm/store_ops.hpp` replace

```cpp
/// in-memory `Db`. They throw the same `bank::` domain errors so the GUI
/// controllers classify failures identically.
```

with

```cpp
/// in-memory `Db`. They throw the same `bank::` domain errors, so a failure
/// reads the same in the browser as natively.
```

Create `examples/bank/wasm/local_setup_wasm.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// localSetup() for the in-memory models: no database to set up, and one worker. The store has no
// locks because the browser runs every model on its one thread; a native run of the same models
// (bank_inmemory_tests) keeps them on one thread too.

#include <string>

#include "local_setup.hpp"

namespace bank::client {

morph::examples::LocalSetup localSetup() {
    return morph::examples::LocalSetup{.setupDatabase = [](std::string const& /*database*/) {}, .workers = 1};
}

}  // namespace bank::client
```

Replace `examples/bank/wasm/CMakeLists.txt` with:

```cmake
# SPDX-License-Identifier: Apache-2.0
#
# The bank client in the browser: ui/main.cpp and the app sources, with Qt
# Quick the only frontend, compiled against the in-memory models. Lightweight
# (ODBC/SQLite) cannot run in a browser, so include/bank/models/*.hpp here
# shadow the native model headers -- the same classes and registrations,
# persisting to bank::wasm::Db -- and come first on the include path, while
# bank/dto and bank/core resolve to the shared headers. bank_inmemory_tests
# (../CMakeLists.txt) runs the same set natively. Added in an Emscripten
# configure with MORPH_BUILD_QT_QUICK=ON.

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2)
qt_standard_project_setup(REQUIRES 6.5)

qt_add_executable(bank
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/main.cpp
    ${BANK_APP_SOURCES}
    ${BANK_INMEMORY_SOURCES}
)
target_include_directories(bank BEFORE PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../app
    ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_link_libraries(bank PRIVATE morph::morph morph::ladder_app_common)
# Qt Quick, and MORPH_EXAMPLE_HAS_TUI=0 / MORPH_EXAMPLE_HAS_QT_QUICK=1: the
# terminal UI is never built under Emscripten.
morph_example_frontends(bank PRIVATE)
target_compile_features(bank PRIVATE cxx_std_23)
apply_bigobj(bank)
```

In `examples/bank/CMakeLists.txt`, place (a) — directly after the `set(BANK_APP_SOURCES …)` block:

```cmake
# The browser build's persistence: the in-memory models behind the shadow
# headers in wasm/include, and the money formatting they share with bank_lib.
set(BANK_INMEMORY_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/wasm/local_setup_wasm.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/wasm/src/models/auth_model_wasm.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/wasm/src/models/account_model_wasm.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/wasm/src/models/customer_model_wasm.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/wasm/src/models/transaction_model_wasm.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/wasm/src/models/card_model_wasm.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/wasm/src/models/payee_model_wasm.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/wasm/src/models/payment_model_wasm.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/wasm/src/models/loan_model_wasm.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/money.cpp
)
```

and (b) — replace the whole `if(EMSCRIPTEN) … endif()` block (its comment included) with:

```cmake
# ── WebAssembly build ────────────────────────────────────────────────────────
# Lightweight (ODBC/SQLite) cannot exist in a browser, so an Emscripten
# configure skips the native stack -- Lightweight, bank_lib, bank_app, the CLI,
# the server and the tests -- and builds only the browser client (wasm/), which
# compiles the same app sources against the in-memory models.
if(EMSCRIPTEN)
    if(MORPH_BUILD_QT_QUICK)
        add_subdirectory(wasm)
    else()
        message(STATUS "examples/bank: the browser client needs MORPH_BUILD_QT_QUICK=ON; not built")
    endif()
    return()
endif()
```

In `.github/workflows/wasm-demo.yml`:

1. Replace the header comment's first two lines with
   `# Builds the bank client for the browser -- examples/bank/ui/main.cpp over the in-memory models in`
   `# examples/bank/wasm -- and publishes it to GitHub Pages under /demo/, alongside the Doxygen docs at the`
   (the remaining two lines stay).
2. Under both `push.paths` and `pull_request.paths`, after `'examples/bank/**'`, add `'examples/common/app/**'`,
   `'src/qt/**'` and `'src/qt_quick/**'`.
3. In both `jurplel/install-qt-action` steps add `modules: qtwebsockets` (MORPH_BUILD_QT needs it; the frontend
   builds on `morph::qt`'s `QtExecutor`).
4. In Configure, replace `-DMORPH_BUILD_BANK_GUI=ON -DMORPH_BUILD_TESTS=OFF` with
   `-DMORPH_BUILD_QT=ON -DMORPH_BUILD_QT_QUICK=ON -DMORPH_BUILD_TESTS=OFF`.
5. In Build, replace `--target bank_gui_wasm` with `--target bank`.
6. Replace the Stage bundle step's `run:` body with:

```bash
          mkdir -p site
          BIN=build-wasm/examples/bank/wasm
          cp "$BIN"/bank.js "$BIN"/bank.wasm "$BIN"/qtloader.js site/
          # GitHub rejects any single file over 100 MB on push; fail here, in the
          # build log, rather than in the deploy step's git-push error.
          ls -la site/
          WASM_BYTES=$(stat -c%s site/bank.wasm)
          echo "bank.wasm: $WASM_BYTES bytes"
          if [ "$WASM_BYTES" -gt 104857600 ]; then
            echo "::error::bank.wasm is $WASM_BYTES bytes, over GitHub's 100 MB push limit"
            exit 1
          fi
          # Serve the Qt loader page as the directory index.
          cp "$BIN"/bank.html site/index.html
```

- [ ] **Step 4: Run the test to verify it passes, and build the browser client where a toolchain exists**

Run: `cmake -S . -B build/bank && cmake --build build/bank --target bank_inmemory_tests && ctest --test-dir build/bank
-L bank -R 'in-memory' --output-on-failure`
Expected: PASS — 1 test.

With Qt for WebAssembly and its emsdk installed (otherwise `wasm-demo.yml` builds it on the pull request, Task 12):

```bash
source /path/to/emsdk/emsdk_env.sh && export EM_CACHE="$PWD/.emcache"
/path/to/qt6-wasm/bin/qt-cmake -S . -B build-wasm -G Ninja -DCMAKE_BUILD_TYPE=MinSizeRel \
    -DQT_HOST_PATH=/path/to/qt6-host -DMORPH_BUILD_EXAMPLES=ON -DMORPH_BUILD_BANK_EXAMPLE=ON \
    -DMORPH_BUILD_QT=ON -DMORPH_BUILD_QT_QUICK=ON -DMORPH_BUILD_TESTS=OFF
cmake --build build-wasm --target bank
python3 -m http.server -d build-wasm/examples/bank/wasm 8000   # open bank.html: signed in as Demo User
```

Mutation check: in `examples/bank/wasm/src/models/transaction_model_wasm.cpp`, make `execute(dto::Deposit const&)`
throw `bank::ValidationError{"broken"}`. Expected FAIL in "the in-memory models serve the seeded demo and a
deposit" (the seed's opening deposit fails; it never signs in). Restore.

- [ ] **Step 5: Commit**

```bash
git add examples/bank/wasm examples/bank/tests/inmemory examples/bank/CMakeLists.txt .github/workflows/wasm-demo.yml
git commit -m "wip(bank): the browser build is the same main over the in-memory models

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 11: Remove the QML client and `MORPH_BUILD_BANK_GUI`; the docs and CI that named them

Every behaviour the QML client's tests pinned now has a controller or view test (Tasks 1–8), so `gui/`, its two
test binaries and the option go, together with every sentence and flag that names them.

**Files:**
- Delete: `examples/bank/gui/` (13 QML files, 7 controllers, `Format.hpp`, `BankClient.*`, `Theme.hpp`, `main.cpp`,
  `CMakeLists.txt`), `examples/bank/tests/gui/` (`test_bank_gui_format.cpp` — ported in Task 1;
  `test_bank_gui_qml_behaviour.cpp` — ported in Tasks 4 and 6; `test_bank_qml_surface.cpp` — dropped with the QML
  it audited)
- Modify: `examples/bank/CMakeLists.txt` — the `# ── Qt 6 GUI (opt-in)` block, the `bank_gui_tests` /
  `bank_gui_qml_tests` block, three comments
- Modify: `CMakeLists.txt` — line 28 (the option) and line 747 (a comment)
- Modify: `CMakePresets.json` — `windows-everything` and `linux-everything`
- Modify: `.github/workflows/ci.yml` — the `bank-sanitizers` job, `linux-all-features`' Configure, the `clang-tidy`
  job's Configure; `.github/workflows/nightly-slow-checks.yml` — line 256
- Modify: `scripts/check_coverage_objects.sh` — line 109
- Modify: `examples/bank/README.md`, `examples/TESTING.md`, `examples/LADDER.md`, `README.md`
- Test: the grep below, the configure warning, and `ctest -L bank`

**Interfaces:**
- Consumes: everything Tasks 1–10 produced (the replacement targets the docs and CI now name).
- Produces: none — after this task no file outside `CHANGELOG.md`, `docs/superpowers/` and `docs/analysis/` names
  the removed client.

- [ ] **Step 1: Write the failing check**

```bash
git grep -nE 'MORPH_BUILD_BANK_GUI|bank_gui_(tests|qml_tests|lib|wasm)|examples/bank/gui(/|_wasm|\b)|bank/gui_wasm|bank.s .gui_wasm|tests/gui/' -- \
    CMakeLists.txt CMakePresets.json .github scripts examples/bank examples/TESTING.md examples/LADDER.md README.md
```

The check passes when it prints nothing. (`ci.yml`'s quoted clang-tidy report naming
`test_bank_gui_qml_behaviour.cpp` is a dated record of one CI run and does not match.)

- [ ] **Step 2: Run it to verify it fails**

Run the command above.
Expected: FAIL — matches in each listed file, among them `CMakeLists.txt:28:option(MORPH_BUILD_BANK_GUI …`,
`CMakePresets.json:105`, `.github/workflows/ci.yml:1284`, `scripts/check_coverage_objects.sh:109` and
`examples/bank/CMakeLists.txt:185`.

- [ ] **Step 3: Remove and rewrite**

Delete the client and its suites:

```bash
git rm -r examples/bank/gui examples/bank/tests/gui
```

**`examples/bank/CMakeLists.txt`.** Delete the four-line block
`# ── Qt 6 GUI (opt-in) ──…` / `if(MORPH_BUILD_BANK_GUI)` / `add_subdirectory(gui)` / `endif()`. Delete the block from
`        # ── bank_gui_tests: the GUI's own suite ──…` through the `endif()` that closes `if(TARGET bank_gui_lib)`
(the one directly after `catch_discover_tests(bank_gui_qml_tests …)`). In the `ladder_bank_server` comment replace

```cmake
# Gated on Qt because the transport is morph::qt's QtWebSocketServer, exactly
# as the macro's version is; MORPH_BUILD_BANK_GUI is *not* the gate, since this
# is a headless server that needs Qt6::Core and no GUI.
#
# The gate is the *option*, not `TARGET morph::qt`: the root CMakeLists adds
# examples/bank (line ~523) well before it creates morph::qt (line ~596), so a
# TARGET check here is false even when Qt is on. Naming the target in
# target_link_libraries is fine regardless -- CMake resolves link names at
# generate time, by which point the alias exists. bank/gui/CMakeLists.txt
# depends on the same ordering.
```

with

```cmake
# Gated on Qt because the transport is morph::qt's QtWebSocketServer, exactly
# as the macro's version is; it is a headless server that needs Qt6::Core and
# no frontend.
#
# The gate is the *option*, not `TARGET morph::qt`: the root CMakeLists adds
# examples/bank well before it creates morph::qt, so a TARGET check here is
# false even when Qt is on. Naming the target in target_link_libraries is fine
# regardless -- CMake resolves link names at generate time, by which point the
# alias exists. The bank binary's morph::qt_quick link relies on the same.
```

and, inside the block, replace

```cmake
    # one -- the same call bank_gui_qml_tests makes further down for the same
    # reason. WebSockets comes along because morph::qt's INTERFACE link
```

with

```cmake
    # one. WebSockets comes along because morph::qt's INTERFACE link
```

In the tests block replace `# LABELS "bank" on all three bank suites.` with `# LABELS "bank" on every bank suite.`
and `# No RESOURCE_LOCK on any of the three calls, and that is a decision` with
`# No RESOURCE_LOCK on any of these calls, and that is a decision`.

**`CMakeLists.txt`.** Delete line 28,
`option(MORPH_BUILD_BANK_GUI      "Build the Qt 6 GUI for the bank example" OFF)`. On line 747 replace
`# single-threaded WASM build (which only wants the bank GUI target).` with
`# single-threaded WASM build (which only wants the bank's browser client).`

**`CMakePresets.json`.** Delete `"MORPH_BUILD_BANK_GUI": "ON",` from `windows-everything` and `linux-everything`.
Then `grep -n 'MORPH_BUILD_TUI\|MORPH_BUILD_QT_QUICK' CMakePresets.json`: Parts 3 and 4 put both in each
`*-everything` preset; add `"MORPH_BUILD_TUI": "ON",` / `"MORPH_BUILD_QT_QUICK": "ON",` after
`"MORPH_BUILD_BANK_EXAMPLE": "ON",` in any of the two that lacks one.

**`scripts/check_coverage_objects.sh`.** Replace `bank_tests|bank_gui_tests|bank_gui_qml_tests)` with
`bank_tests|bank_app_tests|bank_inmemory_tests|bank_smoke_tests)`.

**`.github/workflows/ci.yml`, `bank-sanitizers`.** First run `grep -n 'MORPH_BUILD_TUI' .github/workflows/ci.yml`:
if the job Part 3 enabled the terminal frontend in has a step for its dependencies (libunicode's UCD download
cache), copy that step into `bank-sanitizers` before its Configure step. Then:

1. Replace the header-comment paragraph from `  # What that covers, stated exactly, because the obvious claim is too
   strong.`
   through `  # bounded by how much of bank they drive.` with:

   ```yaml
     # What that covers, stated exactly, because the obvious claim is too strong.
     # An out-of-range double -> std::int64_t conversion in
     # examples/bank/app/format.hpp's parseMinor -- on the path of every amount
     # typed into the bank client -- exits bank_app_tests 1 here only because a
     # test calls parseMinor with such a value: a sanitizer leg reaches only
     # what the suites drive, and this one's reach is bounded by how much of
     # bank they drive.
   ```

2. In the next paragraph replace `Two measured reasons.` with `Two reasons.`, replace `(1) That leg builds no Qt, and
   its own comment reserves the matrix against GUI
   stacks; bank's GUI is where this class of UB lives, so covering it means adding Qt there.` with `(1) That leg
   builds no Qt, and its own comment reserves the matrix against GUI stacks; this job builds Qt Quick for the bank
   binary and its offscreen smoke test.`, and replace its measured `(2)` sentences (from `(2) Cold, cacheless` to
   `the matrix's.`) with `(2) Folding it in would lengthen the slowest leg of a three-leg matrix, whose duration is
   then the matrix's.`
3. Replace the paragraph from `  # The bill was measured: turning bank on` through
   `  # suppressed here and there is no allowlist entry.` with
   `  # Nothing is suppressed here and there is no allowlist entry.`
4. Replace the Configure step's comment and name and the `-DMORPH_BUILD_BANK_GUI=ON \` line:

   ```yaml
         # MORPH_BUILD_TUI and MORPH_BUILD_QT_QUICK: bank_app -- the controllers
         # and format.hpp, the numeric-conversion surface this job exists for --
         # builds without them, but the bank binary and bank_smoke_tests exist
         # only when a frontend does, and the smoke tests run the application
         # end to end on each. No MORPH_BUILD_LADDER: bank is not a rung, so it
         # would add every rung's tree for nothing.
         - name: Configure (clang-ubsan, bank example + both frontends)
           run: |
             cmake --preset clang-ubsan \
               -DMORPH_BUILD_QT=ON \
               -DMORPH_BUILD_BANK_EXAMPLE=ON \
               -DMORPH_BUILD_TUI=ON \
               -DMORPH_BUILD_QT_QUICK=ON \
   ```

   (the compiler and launcher lines after it stay).
5. In the Build step's comment replace `it is *not* bank's three suites, which` with `it is *not* bank's suites,
   which`.
6. Replace the whole comment above `      - name: Every ctest binary is instrumented` (from
   `      # The same assertion linux-sanitizers, ladder-sanitizers and kanban-tsan` through
   `      # all nine there whether or not the variable is set.`) with:

   ```yaml
         # The same assertion linux-sanitizers, ladder-sanitizers and kanban-tsan
         # make. It is not a formality here: every bank target needs an
         # apply_sanitizers() call of its own, and this sweep is what says so
         # rather than letting the leg report a clean bank run over
         # uninstrumented binaries.
         #
         # QT_QPA_PLATFORM=offscreen, because the sweep's first act is
         # `ctest --show-only=json-v1`, which runs every PRE_TEST discovery in
         # this step's environment: a Qt-linked suite that needs a platform
         # plugin to list its tests aborts there without it, and Catch2's
         # CatchAddTests.cmake then fails the whole listing, not that suite's
         # share. The Test step below declares the same value, because the
         # sweep's subject is the binaries that step runs.
   ```

7. In the Test step's comment replace `against 28 tests and 6s for bank's own three suites.` with
   `against seconds for bank's own suites.`

**`.github/workflows/ci.yml`, `linux-all-features` Configure.** Replace the comment paragraph from
`          # MORPH_BUILD_BANK_GUI (with MORPH_BUILD_BANK_EXAMPLE, which it needs` through
`          # meta-gates.` with:

```yaml
          # MORPH_BUILD_BANK_EXAMPLE builds the bank client too: bank_app and
          # its tests always, and the bank binary and its smoke tests because
          # MORPH_BUILD_TUI and MORPH_BUILD_QT_QUICK are on here. This leg can
          # host both: it installs Qt ${{ env.QT_VERSION }} from aqtinstall (Qt
          # Quick needs 6.5+, which the distro Qt other bank jobs use cannot
          # give) and the ODBC/SQLite/yaml-cpp/libzip set the bank example's
          # Lightweight fetch requires.
```

and delete the `-DMORPH_BUILD_BANK_GUI=ON \` line; if the command has no `-DMORPH_BUILD_TUI=ON \` and
`-DMORPH_BUILD_QT_QUICK=ON \` yet (Parts 3–4 add them), put them in its place.

**`.github/workflows/ci.yml`, `clang-tidy` Configure.** Replace the paragraph from
`      # MORPH_BUILD_BANK_GUI is the second flag "every optional feature is ON` through
`      # on a line that carries a finding. Clearing them is separate work from` … `deliberately not folded in here.`
with:

```yaml
      # MORPH_BUILD_TUI and MORPH_BUILD_QT_QUICK are what put bank's
      # ui/main.cpp and tests/smoke/ in this job's database: without a
      # frontend the bank binary and its smoke tests are not configured, and
      # the filter below would drop their sources unanalysed. bank_app and its
      # other tests are in the database either way.
```

and treat the `-DMORPH_BUILD_BANK_GUI=ON \` line as in `linux-all-features`.

**`.github/workflows/nightly-slow-checks.yml`.** Treat line 256 the same way.

**`examples/bank/README.md`.**

1. Replace `It ships the models, a full test suite, a scripted CLI driver, and a **Qt 6` /
   `desktop GUI** — which also builds to **WebAssembly** and runs entirely in the browser.` with
   `It ships the models, a full test suite, a scripted CLI driver, and **one client** that runs in a` /
   `terminal or a Qt Quick window — and builds to **WebAssembly**, running entirely in the browser.`
2. In the demo call-out replace `First load fetches a ~31 MB \`.wasm\`, so` with `First load fetches the \`.wasm\`,
   so`.
3. Replace `is \`wasm-demo.yml\`, and only its WebAssembly GUI.` with `is \`wasm-demo.yml\`, and only its WebAssembly
   client.`
4. Replace the paragraph `Conventions bank **shares** with the rungs: …` (through
   `` `SimulatedRemoteBackend` in `test_remote.cpp` plus the scenario corpus below. ``) with:

   ```markdown
   Conventions bank **shares** with the rungs: persistence exclusively through the
   Lightweight ORM, with the schema owned by `LIGHTWEIGHT_SQL_MIGRATION`
   definitions — [`LADDER.md`](../LADDER.md) calls that "bank's pattern" and binds
   every rung to it — models as the application, plain aggregates on the wire, and
   a toolkit-free client on an injected frontend. Conventions it does **not**
   follow: no schema-driven forms (its client hand-writes one controller per
   screen), and no [`TESTING.md`](../TESTING.md) dual-deployment-mode rig — its
   remote coverage is `SimulatedRemoteBackend` in `test_remote.cpp` plus the
   scenario corpus below.
   ```

5. Replace the whole `### Qt 6 QML GUI` section (heading through the `BANK_GUI_SMOKE` paragraph) with:

   ````markdown
   ### The client: one app, any frontend

   The client is **`bank_app`** (`app/`), a toolkit-free library, and **`bank`** (`ui/main.cpp`), the
   one binary that runs it.

   - **Controllers** (`app/controllers/`), one per screen: `AuthController`, `AccountsController`,
     `TransactionsController`, `CardsController`, `PayeesController`, `LoansController`. Each keeps what
     the user picked and typed in a `morph::reactive::Store`, reads through `Query`s keyed on the session
     (signing out clears every screen), writes through `Mutation`s that name the queries they invalidate,
     and turns results into display-ready rows in `Computed`s; money is formatted in `app/format.hpp`,
     never in a view. Failures, and the `Transaction posted` / `Bill paid` confirmations, go to one shared
     notice strip (`app/notices.hpp`).
   - **Views** (`app/views/`), one `morph::ui` tree per screen, bindings only. `BankApplication`
     (`app/bank_application.hpp`) owns the controllers and shows the login screen or the shell: a menu,
     the page heading and a switch over the current page. Opening a page fetches it again.
   - **Toolkit-free by construction:** `bank_app` links `morph`, `bank_lib` and `examples/common`'s app
     layer and nothing that brings Qt or a terminal; the configure fails if it links one, and
     `app/toolkit_free.cpp` stops the build if a Qt module reaches its compile line.
   - **`bank`** is built when `MORPH_BUILD_TUI` or `MORPH_BUILD_QT_QUICK` is on and links whichever are.
     It picks one at run time: `--ui=tui` or `--ui=qt`, else `MORPH_UI`, else Qt Quick where a display is
     available, else the terminal.

   ```sh
   cmake -G Ninja -B build -S . -DMORPH_BUILD_BANK_EXAMPLE=ON -DMORPH_BUILD_TUI=ON \
         -DMORPH_BUILD_QT=ON -DMORPH_BUILD_QT_QUICK=ON -DMORPH_BUILD_NET=ON
   cmake --build build --target bank
   ./build/examples/bank/bank                                 # local; database in the temp directory
   ./build/examples/bank/bank --ui=tui --db bank.db           # the terminal, a database of your own
   ./build/examples/bank/bank --seed                          # signed in as demo / demo1234
   ./build/examples/bank/bank --server ws://127.0.0.1:54321   # against ladder_bank_server (above)
   ```

   Remote mode uses `QtWebSocketBackend` on Qt Quick and `morph::net`'s socket backend on the terminal;
   the latter needs `MORPH_BUILD_NET` and is POSIX-only, so a Windows terminal client runs locally only.

   **What looks different from the old QML client.** Behaviour is kept; the look is each frontend's own.

   - No custom components: panels, buttons and pickers are the frontend's, and status pills are coloured
     text. `Active` and `Open` take the success tone; `Frozen` and `Cancelled` both take the error tone,
     because the shared palette has no warning tone.
   - Accounts are a vertical list rather than a flow of cards. The sidebar is a menu; the current page is
     named by the heading rather than by a highlighted entry.
   - A button whose input is incomplete or invalid is disabled, where the QML button stayed enabled and
     answered with an error notice (`Enter a valid amount.`, `Pick an account.`, …).
   - The transfer amount field is labelled `Transfer amount`; a loan's rate and term must be whole numbers.
   - There is no screenshot mode (`BANK_GUI_SMOKE`); `bank_smoke_tests` runs the application on both
     frontends instead.

   Kept, and tested (see [Tests](#tests)): the notice texts and their 3.2 seconds; the account the
   move-money picker names is the account the next deposit lands in, across the refresh every deposit
   causes; every screen fetches again when opened; a blank or unparseable overdraft or card limit means
   none.
   ````

6. Replace the `### WebAssembly demo (self-contained, GitHub Pages)` section's body after its first paragraph
   (from `` `gui_wasm/` is a **single-threaded WebAssembly** build `` through the paragraph ending
   `` gated `if(NOT EMSCRIPTEN)` in `CMakeLists.txt`. ``) with:

   ````markdown
   It is the same `ui/main.cpp` and the same app sources, built single-threaded for WebAssembly with Qt
   Quick as the only frontend, running **entirely in the browser** — the morph model layer is the "server
   in the background", with no external process. Lightweight (ODBC/SQLite) can't run in a browser, so
   `wasm/` swaps persistence for an **in-memory store** (`wasm/include/bank/wasm/`) behind **shadow model
   headers** that come ahead of the native ones on the include path; the controllers, views and DTOs
   compile unchanged. `bank_inmemory_tests` compiles the same set natively and runs the seeded demo and a
   deposit through it. Single-threaded ⇒ no SharedArrayBuffer ⇒ **no COOP/COEP headers**, so plain GitHub
   Pages hosts it. The page always starts with `--seed`'s demo user and accounts.

   Build locally (needs Qt-for-WASM + a matching emsdk):

   ```
   source /path/to/qt6-wasm/emsdk/emsdk_env.sh
   export EM_CACHE="$PWD/.emcache"
   /path/to/qt6-wasm/bin/qt-cmake -S . -B build-wasm -G Ninja \
     -DMORPH_BUILD_EXAMPLES=ON -DMORPH_BUILD_BANK_EXAMPLE=ON \
     -DMORPH_BUILD_QT=ON -DMORPH_BUILD_QT_QUICK=ON -DMORPH_BUILD_TESTS=OFF
   cmake --build build-wasm --target bank
   python3 -m http.server -d build-wasm/examples/bank/wasm 8000   # open bank.html
   ```

   CI (`.github/workflows/wasm-demo.yml`) builds the bundle with a matched host+wasm Qt pair and publishes
   it to `…github.io/<repo>/demo/`, coexisting with the Doxygen docs at the site root. The native build is
   unaffected — the native stack is gated `if(NOT EMSCRIPTEN)` in `CMakeLists.txt`.
   ````

   In the section's first paragraph replace `the first load fetches a ~31 MB \`.wasm\`` with
   `the first load fetches the \`.wasm\``.
7. In `## Tests`, replace everything from `` `tests/gui/` is a second binary, `bank_gui_tests`, `` through the end
   of the `bank_gui_qml_tests` code block with:

   ````markdown
   The client has three more binaries, all labelled `bank` (`ctest -L bank`):

   - **`bank_app_tests`** (`tests/app/`) — the controllers, headless: each runs over a local bridge on
     `examples/common`'s `FakeAppContext` (a `MainThreadExecutor` and a manual scheduler) and waits by
     pumping, never by sleeping. It carries what the QML suites held — `parseMinor`'s cases, the move-money picker
     sequence, the posted and paid notices — and the case the old controller could not pass: select
     account A, then B, and A's late history reply must not replace B's history. Each screen also has a
     view test on `ui::testing::RecordingBackend` that drives its controls and compares the tree dump
     with `tests/app/golden/<screen>.txt`; after an intended change, regenerate with
     `BANK_UPDATE_GOLDEN=1` and review the diff.
   - **`bank_inmemory_tests`** (`tests/inmemory/`) — the app over the browser build's in-memory models,
     natively.
   - **`bank_smoke_tests`** (`tests/smoke/`) — the application mounts and quits on each frontend the
     configure built: the terminal through scripted input, Qt Quick offscreen. Built only with a frontend,
     as a binary of its own, because the Qt Quick frontend constructs its own `QGuiApplication`.

   ```sh
   cmake --build build --target bank_app_tests bank_inmemory_tests bank_smoke_tests
   ctest --test-dir build -L bank --output-on-failure
   ```
   ````

8. Replace the `## Status` paragraph with:

   ```markdown
   Models, tests, CLI, the client (terminal and Qt Quick, one binary, local or remote), and a
   self-contained WebAssembly build hosted on GitHub Pages are complete. Possible extensions: durable
   in-browser persistence (IDBFS/OPFS) for the WASM build, and switching its in-browser backend to
   `RemoteServer` + `SimulatedRemoteBackend` to surface the JSON wire protocol.
   ```

**`examples/TESTING.md`.**

1. Replace `` QProcess client harness, the Qt-owning Catch2 `main()`), the pump helpers in `` /
   `` `examples/bank/tests/bank_test_support.hpp`, and the presenter shape of `` /
   `` `examples/bank/gui/controllers/`. `` with `` QProcess client harness, the Qt-owning Catch2 `main()`), and the ``
   /
   `` pump helpers in `examples/bank/tests/bank_test_support.hpp`. ``
2. Replace the bullet `- There are **zero GUI tests** in the repo today. Bank's controllers are …` (five lines,
   through `` sleep-pumped screenshot smoke inside `gui/main.cpp`. ``) with
   `` - Bank's client is a toolkit-free app (`examples/bank/app/`): its controllers are tested headless in `` /
   `` `bank_app_tests` over a local bridge, and `bank --server` reaches `ladder_bank_server` over a socket. ``
3. Replace `` access (`bank`'s `AppShell.qml` calls `controllers[current].refresh()` `` /
   `through a \`var\` array, so five reachable invokables sweep as unreferenced);` with
   `access (an invokable reached only through a \`var\` array sweeps as unreferenced);`
4. Replace `` `ledger`, `lims` and `kanban` — and by `examples/bank`, which is not a rung. `` with
   `` `ledger`, `lims` and `kanban`. ``
5. Delete the three paragraphs from `` `bank` is where the two shell shapes diverge, `` through
   `no-platform-plugin property.` (the blank line after them stays).
6. Replace `` executor itself: the **WASM constraint-parity mode** (exactly bank's `` / `` `__EMSCRIPTEN__` wiring). ``
   with `executor itself: the **WASM constraint-parity mode** (what` / `` `examples::connect` builds under Emscripten).
   ``
7. Replace ``- Do **not** copy bank's `gui_wasm` shadow-header pattern`` with
   ``- Do **not** copy bank's `wasm/` shadow-header pattern``.

**`examples/LADDER.md`.** Replace `` no schema-driven forms, no `examples/common/gui` presenter, and no dual-mode `` /
`` [`TESTING.md`](TESTING.md) rig (it does use the shared testkit's QML surface `` / `audit).` with
`` no schema-driven forms and no dual-mode [`TESTING.md`](TESTING.md) rig; its `` /
`client is a toolkit-free app on an injected frontend, as every rung's is.`

**`README.md`** (line 523–525). Replace `` DTO/entity layers, a Qt desktop GUI, and a self-contained single-threaded Qt
`` /
`**WASM** build.` with `DTO/entity layers, one client for the terminal and Qt Quick, and the same client` /
`built as a self-contained single-threaded **WASM** page.`

- [ ] **Step 4: Run the check, the build and the suites**

```bash
git grep -nE 'MORPH_BUILD_BANK_GUI|bank_gui_(tests|qml_tests|lib|wasm)|examples/bank/gui(/|_wasm|\b)|bank/gui_wasm|bank.s .gui_wasm|tests/gui/' -- \
    CMakeLists.txt CMakePresets.json .github scripts examples/bank examples/TESTING.md examples/LADDER.md README.md
cmake -S . -B build/bank -DMORPH_BUILD_BANK_GUI=ON 2>&1 | grep -A2 'not used by the project'
cmake --build build/bank && ctest --test-dir build/bank -L bank --output-on-failure
pre-commit run markdownlint --files examples/bank/README.md examples/TESTING.md examples/LADDER.md README.md
```

Expected: the grep prints nothing; the configure reports `MORPH_BUILD_BANK_GUI` among the manually-specified
variables not used by the project (the option is gone); every `bank`-labelled test passes (`bank_tests`,
`bank_app_tests` 52, `bank_inmemory_tests` 1, `bank_smoke_tests` 2); markdownlint is clean.

Mutation check: put `-DMORPH_BUILD_BANK_GUI=ON \` back into `nightly-slow-checks.yml`. Expected: the grep prints
that line. Remove it again.

- [ ] **Step 5: Commit**

```bash
git add -A examples/bank CMakeLists.txt CMakePresets.json .github scripts examples/TESTING.md examples/LADDER.md README.md
git commit -m "wip(bank): remove the QML client and MORPH_BUILD_BANK_GUI

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 12: Whole-part verification

**Files:** none new; a fix lands in the file it concerns.

- [ ] **Step 1: The full build and every bank suite**

```bash
cmake --build build/bank && ctest --test-dir build/bank --output-on-failure
ctest --test-dir build/bank -L bank --output-on-failure
```

Expected: everything passes — the whole configure's suites, not only bank's; `-L bank` lists `bank_tests`'
cases, 52 `bank_app_tests` cases, 1 `bank_inmemory_tests` case and 2 `bank_smoke_tests` cases.

- [ ] **Step 2: The app builds and tests without any frontend, and the binary needs one**

```bash
cmake -S . -B build/bank-nofe -G Ninja -DCMAKE_BUILD_TYPE=Debug -DMORPH_BUILD_BANK_EXAMPLE=ON
cmake --build build/bank-nofe --target bank_app_tests bank_inmemory_tests
ctest --test-dir build/bank-nofe -L bank --output-on-failure
cmake --build build/bank-nofe --target bank
```

Expected: the two suites build and pass with `MORPH_BUILD_TUI` and `MORPH_BUILD_QT_QUICK` off (spec 4 §6:
controller and view tests need neither frontend); the last command fails with `unknown target 'bank'`.

- [ ] **Step 3: The ladder configure, where examples/ adds the app layer**

```bash
cmake -S . -B build/all -G Ninja -DCMAKE_BUILD_TYPE=Debug -DMORPH_BUILD_TUI=ON -DMORPH_BUILD_QT=ON \
      -DMORPH_BUILD_QT_QUICK=ON -DMORPH_BUILD_LADDER=ON -DMORPH_LADDER_RUNGS=all \
      -DMORPH_BUILD_BANK_EXAMPLE=ON -DMORPH_BUILD_NET=ON -DMORPH_BUILD_FORMS_QML=ON
cmake --build build/all && ctest --test-dir build/all -L bank --output-on-failure
```

Expected: configures (the root adds `examples/common/app` once, for the ladder and bank alike), builds, and every
bank suite passes.

- [ ] **Step 4: Sanitizers, as CI's `bank-sanitizers` job runs them** (Linux)

```bash
cmake --preset clang-ubsan -DMORPH_BUILD_QT=ON -DMORPH_BUILD_BANK_EXAMPLE=ON \
      -DMORPH_BUILD_TUI=ON -DMORPH_BUILD_QT_QUICK=ON
QT_QPA_PLATFORM=offscreen cmake --build --preset clang-ubsan
QT_QPA_PLATFORM=offscreen bash scripts/check_sanitizer_instrumentation.sh build/clang-ubsan ubsan
QT_QPA_PLATFORM=offscreen UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
    ctest --preset clang-ubsan -L bank --output-on-failure
cmake --preset clang-asan -DMORPH_BUILD_BANK_EXAMPLE=ON
cmake --build --preset clang-asan --target bank_app_tests bank_inmemory_tests
bash scripts/check_sanitizer_instrumentation.sh --binary build/clang-asan/examples/bank/bank_app_tests asan
bash scripts/check_sanitizer_instrumentation.sh --binary build/clang-asan/examples/bank/bank_inmemory_tests asan
./build/clang-asan/examples/bank/bank_app_tests && ./build/clang-asan/examples/bank/bank_inmemory_tests
```

Expected: the sweep reports every ctest binary carrying `__ubsan_` symbols; no UBSan or ASan finding. The ASan
run is what observes the gates this part relies on: a reply arriving after a controller is gone (`CallbackScope`
last), a hide timer after `Notices` is gone (`TimerHandle` last), a seed reply after the application is gone. On
macOS use an ASan configure of the same tree in place of the presets.

- [ ] **Step 5: clang-tidy over the changed lines** — CONTRIBUTING's recipe, "Running the `clang-tidy-diff` gate
  locally", on a configure with `-DMORPH_BUILD_BANK_EXAMPLE=ON -DMORPH_BUILD_TUI=ON -DMORPH_BUILD_QT=ON
  -DMORPH_BUILD_QT_QUICK=ON` added, over this part's paths:

```bash
git fetch origin master
git diff -U0 origin/master...HEAD -- examples/bank > /tmp/bank.diff
files=$(grep -c '^+++ ' /tmp/bank.diff)
test "$files" -gt 0 || { echo "no changed files -- wrong diff base?"; exit 1; }
echo "clang-tidy-diff over $files changed file(s)"
python3 "$(find /usr/lib/llvm-*/share/clang /usr/share/clang -name clang-tidy-diff.py | head -1)" \
    -p1 -path build/clang-debug -j "$(nproc)" -quiet \
    -extra-arg=-std=c++23 -extra-arg=-Wno-missing-include-dirs < /tmp/bank.diff
```

Expected: the file count is printed and non-zero (about 60 files), and no finding.

- [ ] **Step 6: What this part does not need** — no header under `include/morph` changed and no CMake component
  was added, so the Docs build and `scripts/check_install_export.sh` have nothing new to check; the WebAssembly
  build is `wasm-demo.yml`'s job on the pull request when no local Qt-for-WASM toolchain ran Task 10 Step 4. Say
  which in the hand-off.

- [ ] **Step 7: Commit any fixes**

```bash
git add -A examples/bank .github CMakeLists.txt CMakePresets.json scripts
git commit -m "wip(bank): fixes from the verification gates

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

Skip the commit if there was nothing to fix, and say so in the hand-off.

---

### Task 13: Squash this part into its one commit

- [ ] **Step 1: Write the message**

Write `/tmp/bank-message.txt`:

```text
examples/bank: one app, any frontend

The bank client is bank_app, a toolkit-free library: six controllers
(Auth, Accounts, Transactions, Cards, Payees, Loans) built from Store,
Query and Mutation, a morph::ui view per screen, and BankApplication.
One binary, bank, runs it on the terminal or Qt Quick, locally or
against ladder_bank_server; the browser build is the same main over the
in-memory models. The move-money history is a Query keyed on the
selection, so a late reply for a previously selected account no longer
replaces the one shown. The QML client and MORPH_BUILD_BANK_GUI are gone.

Signed-off-by: Christian Parpart <christian@parpart.family>
```

Add one paragraph before the trailer for the deviation this plan makes on purpose: Part 6 expects bank's binary to
use `morph_add_example_ui`; it uses `morph_example_frontends` directly, because that helper calls `apply_warnings()`
and bank's targets cannot — the model headers include the Lightweight ORM's, which are not `-Werror` clean. If
Task 9's manual runs or Task 12 found something else the plan did not say, add one paragraph per item as well: what
the plan said, what was done instead, and why (master plan, "The docs commit").

- [ ] **Step 2: Squash**

Follow the master plan's "Squashing a part" procedure with `key=bank` and `git commit -F /tmp/bank-message.txt`.

Expected: `git diff --exit-code "$pre" HEAD` prints `squash preserved the tree`, and `git log --oneline master..HEAD`
ends with `examples/bank: one app, any frontend` directly after
`examples/common: Qt-free app environment, transport, poller and test waits; gallery and workout`.

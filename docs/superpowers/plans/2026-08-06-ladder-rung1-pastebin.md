# Ladder Rung 1 (Pastebin) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build rung 1 of the [application ladder](../../../examples/LADDER.md) —
**pastebin**: one entity (`PasteRecord`), one model (`PasteModel`), full
local/remote loop, desktop + WASM clients, per
[`examples/pastebin/README.md`](../../../examples/pastebin/README.md) (design
questions already resolved in that file — read it first, it is this plan's
design authority).

**Architecture:** `ladder_pastebin_lib` (STATIC: DTOs, entity, migration,
model, app bootstrap — morph + Lightweight, no Qt/Catch2), `ladder_pastebin_gui_lib`
(STATIC: presenters + the rung-owned forms-controller glue — `Qt6::Core` only,
no `Qt6::WebSockets`, no Catch2), `ladder_pastebin_gui` (EXE: Qt Widgets/QML
desktop client), `ladder_pastebin_gui_wasm` (EXE, Emscripten only), a
standalone `ladder_pastebin_server` (EXE: hosts `PasteModel` over
`QtWebSocketServer` for the WASM/remote clients and the browser smoke),
and `ladder_pastebin_tests` (EXE: Catch2 model + presenter tests, full
`BackendRig` mode matrix). `morph_add_rung()` (`cmake/morph_add_rung.cmake`,
currently a stub) gets its real implementation in Task 8, generalized enough
that rung 2 reuses it unchanged.

**Tech Stack:** C++23, Qt6 (Core, WebSockets, Quick/QuickControls2), Catch2 v3,
Lightweight ORM (SQLite/ODBC), CMake 3.25+, `morph::forms` +
`MorphForms` QML module, `morph::journal::FileActionLog`.

## Global Constraints

- C++23 throughout (`target_compile_features(... PUBLIC cxx_std_23)`).
- **DTO type discipline** ([`IMPLEMENTATION.md`](../../../examples/IMPLEMENTATION.md)
  rule 3): the only plain type permitted in an action/result field is
  `std::string` (paste content, syntax label). Everything else is a strong
  type — `PasteId`, `morph::time::Timestamp`, `enum class`, a reads
  `Quantity`. **No `int`/`int64_t`/`double`/`float`/`bool`/raw enum in any
  DTO field.**
- **Persistence exclusively through Lightweight** ([`IMPLEMENTATION.md`](../../../examples/IMPLEMENTATION.md)
  rule 4). The one sanctioned exception: `GetPaste`'s atomic burn-after-read
  decrement, via Lightweight's raw-query facility (`SqlStatement::Prepare`/
  `Execute`), the pre-enumerated sanctioned-escape-tier answer — see Task 5.
  No raw `sqlite3_*` calls anywhere.
- **`PasteModel` is registered plain** — no `BRIDGE_MODEL_KEY`/`AllowShared`
  (resolved design decision, README). Every action dispatch gets a fresh
  model instance; all real state lives in the database.
- **Journal**: `GetPaste` is the one client-visible, journaled action
  (default `Loggable::Yes`, not split — resolved design decision, README).
  `ExpirePaste` is dispatched only via the internal-client sweep (Task 6),
  never directly by a GUI client.
- **Time**: model code never calls `morph::time::Timestamp::now()`/
  `DateTime::now()` directly — always `morph::ladder::now()` (Task 1).
- **No `sleep_for` outside `pump.hpp`** — a review-rejectable defect
  ([`TESTING.md`](../../../examples/TESTING.md) "Pumping discipline").
- **Presenters/GUI code take `(Bridge&, IExecutor*)`, never construct
  backends or executors themselves** ([`TESTING.md`](../../../examples/TESTING.md)
  presenter rule 2) — everything composes over `examples/common/gui::AppContext`.
  This is exactly the rule `FormsControllerCore` breaks (finding 021); the
  rung-owned forms-controller glue (Task 10) must not repeat that mistake.
- **Schema-driven GUI, always** ([`IMPLEMENTATION.md`](../../../examples/IMPLEMENTATION.md)
  rule 2): every form renders from `morph::forms::schemaJson<A>()` through
  the real `MorphForms` QML module. No hand-built input widgets.
- Every ladder CMake target wraps its definition in
  `if(AF_COVERAGE) apply_coverage(<target>) endif()`
  ([`TESTING.md`](../../../examples/TESTING.md) "Build system and CI").
- Model coverage target: the measured ceiling, not a blind 100%
  ([`IMPLEMENTATION.md`](../../../examples/IMPLEMENTATION.md) rule 5) —
  document every known llvm-cov artifact line the same way
  `examples/common`'s own `codecov.yml` component does.
- License hygiene: nothing ported from MicroBin/PrivateBin beyond
  requirements/data-shape/behavior; all implementation original.

---

## Task 1: Injectable clock (`examples/common`)

**Files:**
- Create: `examples/common/clock.hpp`
- Create: `examples/common/testkit/test_clock.cpp`
- Modify: `examples/common/CMakeLists.txt` (add the new test file to
  `ladder_common_tests`'s source list)

**Interfaces:**
- Produces: `morph::ladder::now() -> ::morph::time::Timestamp`,
  `morph::ladder::ScopedClockOverride` (RAII, freezes `now()` for its
  lifetime, nests correctly). Every later task's model/sweep code that needs
  the current instant calls `morph::ladder::now()`, never
  `::morph::time::Timestamp::now()`/`DateTime::now()` directly.

This closes the "injectable time source" framework prerequisite
([`LADDER.md`](../../../examples/LADDER.md) framework prerequisite 3) the
way `examples/common/testkit/pump.hpp`'s `computeDeadlineScale` already
established: a process-global, cross-thread-visible override (a model runs
on its own strand/pool thread, not the test thread that installs the
override, so this cannot be `thread_local`).

- [ ] **Step 1: Write `examples/common/clock.hpp`**

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/util/datetime.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>

/// @file
/// The ladder-wide injectable "now" (examples/TESTING.md's framework-gaps
/// item 6; examples/LADDER.md framework prerequisite 3). Registry-constructed
/// models are always default-constructed (docs/findings/003,
/// docs/findings/020), so there is no constructor-injection seam for a
/// clock — every rung's time-dependent model logic reads
/// `morph::ladder::now()` instead of `Timestamp::now()`/`DateTime::now()`
/// directly, and a test overrides the process-global provider for the span
/// it needs.

namespace morph::ladder {

namespace detail {

/// @brief Process-global override, in epoch milliseconds; `-1` means
///        "disabled, read the real wall clock".
[[nodiscard]] inline std::atomic<std::int64_t>& overrideMillisSlot() noexcept {
    static std::atomic<std::int64_t> slot{-1};
    return slot;
}

}  // namespace detail

/// @brief The ladder's injectable "now".
/// @return The real wall-clock instant, or the frozen instant a live
///         `ScopedClockOverride` installed.
[[nodiscard]] inline ::morph::time::Timestamp now() {
    const std::int64_t overrideMs = detail::overrideMillisSlot().load();
    if (overrideMs < 0) {
        return ::morph::time::Timestamp::now();
    }
    return ::morph::time::Timestamp{::morph::time::DateTime{
        std::chrono::sys_time<std::chrono::milliseconds>{std::chrono::milliseconds{overrideMs}}}};
}

/// @brief Freezes `morph::ladder::now()` at a fixed instant for the guard's
///        lifetime; restores the previous override (nests correctly) on
///        destruction.
///
/// Cross-thread visible (a `std::atomic`, not `thread_local`): a model under
/// test runs on its own strand/pool thread, not the test thread that
/// constructs this guard.
class ScopedClockOverride {
  public:
    /// @param frozenAt The instant `now()` reads for the guard's lifetime.
    explicit ScopedClockOverride(::morph::time::DateTime frozenAt) noexcept
        : _previous{detail::overrideMillisSlot().exchange(frozenAt.value.time_since_epoch().count())} {}

    ~ScopedClockOverride() { detail::overrideMillisSlot().store(_previous); }

    ScopedClockOverride(const ScopedClockOverride&) = delete;
    ScopedClockOverride& operator=(const ScopedClockOverride&) = delete;
    ScopedClockOverride(ScopedClockOverride&&) = delete;
    ScopedClockOverride& operator=(ScopedClockOverride&&) = delete;

  private:
    std::int64_t _previous;
};

}  // namespace morph::ladder
```

- [ ] **Step 2: Write `examples/common/testkit/test_clock.cpp`**

```cpp
// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include "common/clock.hpp"

using namespace std::chrono_literals;

TEST_CASE("morph::ladder::now() reads the real wall clock with no override installed",
          "[ladder][testkit][clock]") {
    const auto before = ::morph::time::DateTime::now();
    const auto observed = morph::ladder::now();
    const auto after = ::morph::time::DateTime::now();
    REQUIRE(observed.hasValue());
    REQUIRE(*observed >= before);
    REQUIRE(*observed <= after);
}

TEST_CASE("ScopedClockOverride freezes now() at the given instant", "[ladder][testkit][clock]") {
    const ::morph::time::DateTime frozen{std::chrono::year{2030}, std::chrono::month{1}, std::chrono::day{1},
                                          std::chrono::hours{0},   std::chrono::minutes{0}, std::chrono::seconds{0}};
    {
        morph::ladder::ScopedClockOverride guard{frozen};
        REQUIRE(*morph::ladder::now() == frozen);
        REQUIRE(*morph::ladder::now() == frozen);  // stable across repeated reads, not a one-shot
    }
    REQUIRE(*morph::ladder::now() != frozen);  // restored to the real clock after the guard's scope
}

TEST_CASE("ScopedClockOverride nests: the inner guard wins, the outer resumes on inner's destruction",
          "[ladder][testkit][clock]") {
    const ::morph::time::DateTime outer{std::chrono::year{2030}, std::chrono::month{1}, std::chrono::day{1},
                                         std::chrono::hours{0},   std::chrono::minutes{0}, std::chrono::seconds{0}};
    const ::morph::time::DateTime inner{std::chrono::year{2031}, std::chrono::month{6}, std::chrono::day{15},
                                         std::chrono::hours{12},  std::chrono::minutes{0}, std::chrono::seconds{0}};
    morph::ladder::ScopedClockOverride outerGuard{outer};
    REQUIRE(*morph::ladder::now() == outer);
    {
        morph::ladder::ScopedClockOverride innerGuard{inner};
        REQUIRE(*morph::ladder::now() == inner);
    }
    REQUIRE(*morph::ladder::now() == outer);
}
```

- [ ] **Step 3: Add the new test file to `examples/common/CMakeLists.txt`**

In the `ladder_common_tests` target's `add_executable(...)` source list
(alongside `testkit/test_pump.cpp` etc.), add `testkit/test_clock.cpp`.
`examples/common/clock.hpp` needs no new CMake target of its own — it is a
header consumed via the existing `target_include_directories(... PUBLIC
${CMAKE_CURRENT_SOURCE_DIR})` on `morph_ladder_testkit`/`morph_ladder_gui`
(both already add `${CMAKE_CURRENT_SOURCE_DIR}` — i.e. `examples/common` —
to their include path, so `#include "common/clock.hpp"` — wait, verify the
actual existing `#include` convention: check how `testkit/pump.hpp` is
included from a test file (e.g. `#include "testkit/pump.hpp"` in
`test_backend_rig.cpp`) — that means the include root is `examples/common`
itself, so this new header's own include path is `#include "clock.hpp"` if
placed at `examples/common/clock.hpp` directly (matching `examples/common/gui/`
and `examples/common/testkit/` both being subdirectories) — **place the file
at `examples/common/clock.hpp` (directly in `examples/common/`, not in a
`testkit/`/`gui/` subdirectory)** since it is consumed by both, and include
it elsewhere as `#include "clock.hpp"` from files also directly under
`examples/common/` or `#include "clock.hpp"` resolving via the same include
root other subdirectories use — confirm the exact working form by checking
one existing cross-subdirectory include (e.g. does `gui/presenter.hpp`
include anything from `testkit/`? If no precedent exists, the safe form is
`#include "clock.hpp"`, which resolves correctly from any file compiled
with `examples/common` on its include path, which every ladder target
already has).

- [ ] **Step 4: Build and run**

```bash
cmake --build build/<preset> --target ladder_common_tests
QT_QPA_PLATFORM=offscreen ctest --test-dir build/<preset> -R clock --output-on-failure
```

Expected: 3 new test cases pass, 100% line and branch coverage on
`clock.hpp` (both branches of `now()`'s override check are exercised by the
tests above; no DI extraction needed beyond what's already here since
`overrideMillisSlot()` is a plain runtime atomic, not a once-per-process
static-const guard — pump.hpp's DI pattern doesn't apply here since there is
no such guard to work around).

- [ ] **Step 5: Commit**

```bash
git add examples/common/clock.hpp examples/common/testkit/test_clock.cpp examples/common/CMakeLists.txt
git commit -m "examples/common: add the ladder-wide injectable clock"
```

---

## Task 2: Pastebin core types (units, strong ids, errors)

**Files:**
- Create: `examples/pastebin/include/pastebin/units.hpp`
- Create: `examples/pastebin/include/pastebin/core/types.hpp`
- Create: `examples/pastebin/include/pastebin/core/errors.hpp`

**Interfaces:**
- Produces: `pastebin::Unit` (enum), `pastebin::Reads` (alias for
  `Quantity<Unit::count, 1>`), `pastebin::PasteId` and `pastebin::PasteCursor`
  (strong, `hasValue()`-capable id/cursor types), `pastebin::Ack` (trivial
  result for actions with nothing to return), `pastebin::PastebinError`
  hierarchy (`NotFound`, `Expired`, `Burned`, `ValidationError`, `TooLarge`).
  Every later DTO/model task consumes these exact names.

`PasteId` follows `morph::forms::Ranged<Min,Max,Step>`'s shape
(`include/morph/forms/widget_hints.hpp:70-118`) — the closest existing
`hasValue()`-capable newtype template in the repo (finding 009: no generic
`Tagged<T,"Name">` helper exists yet) — but wraps a `std::string` (the
animal-name id) instead of a bounded arithmetic value, so it needs its own
`glz::meta` specialization (a plain JSON string on the wire, exactly
`Ranged`'s own comment describes for its wrapper family), not `Ranged`
itself.

- [ ] **Step 1: Write `examples/pastebin/include/pastebin/units.hpp`**

Modeled on `examples/forms/lab_units.hpp`'s exact shape (enum +
`UnitTraits<E>::meta`/`relations` specialization + consteval algebra). One
unit is enough for rung 1: a dimensionless "count" for `burnAfterReads`/
`readCount`. `morph::units::Quantity<U, DeclaredDecimals>` requires
`DeclaredDecimals >= 1` (zero is not legal), so this unit's `defaultDecimals`
is `1` even though every value that ever appears is a whole number by
construction — `EditPaste`/`CreatePaste`'s `validate()` (Task 3) enforces the
whole-number constraint explicitly, the DTO type alone cannot.

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/util/quantity.hpp>

/// @file
/// Pastebin's one-unit system: a dimensionless read count. Modeled on
/// examples/forms/lab_units.hpp's shape — see that file for the full
/// UnitTraits/consteval-algebra contract this mirrors.

namespace pastebin {

enum class Unit {
    count,
};

}  // namespace pastebin

template <>
struct morph::units::UnitTraits<pastebin::Unit> {
    [[nodiscard]] static constexpr UnitMeta meta(pastebin::Unit u) {
        switch (u) {
            case pastebin::Unit::count:
                return UnitMeta{.symbol = "", .name = "count", .defaultDecimals = 1};
        }
        return UnitMeta{};
    }
};

namespace pastebin {

/// @brief A whole-number read count (burn-after-N-reads, read_count).
using Reads = ::morph::units::Quantity<Unit::count, 1>;

}  // namespace pastebin
```

**Verify `UnitMeta`'s exact field names/types against
`examples/forms/lab_units.hpp` before writing this** — the shape above is
inferred from the `UnitTraits<decltype(U)>::meta(U).defaultDecimals`
reference in `quantity.hpp`'s `Quantity` definition (already confirmed to
exist as a static member access), but this task's implementer must open
`lab_units.hpp` and copy its `UnitMeta`/`UnitTraits` specialization's real
field names verbatim rather than trust the sketch above if they differ.

- [ ] **Step 2: Write `examples/pastebin/include/pastebin/core/types.hpp`**

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <compare>
#include <optional>
#include <string>
#include <utility>

/// @file
/// PasteId: a hasValue()-capable strong id wrapping the animal-name paste
/// key. Modeled on morph::forms::Ranged's shape
/// (include/morph/forms/widget_hints.hpp) — the closest existing
/// hasValue()-capable newtype template — but wraps a std::string, not a
/// bounded arithmetic value, so it carries its own glz::meta rather than
/// reusing Ranged's. First real consumer of the eventual Tagged<T,"Name">
/// gap (docs/findings/009); do not promote this into a generic helper here
/// — the promotion rule (examples/IMPLEMENTATION.md) triggers on a third
/// consumer, not the first.

namespace pastebin {

struct PasteId {
    /// @brief The payload; `std::nullopt` means "not entered".
    std::optional<std::string> value;

    constexpr PasteId() noexcept = default;

    /// @brief Engages with @p id.
    explicit PasteId(std::string id) noexcept : value{std::move(id)} {}

    /// @brief Adopts an optional payload as-is.
    explicit PasteId(std::optional<std::string> payload) noexcept : value{std::move(payload)} {}

    [[nodiscard]] bool hasValue() const noexcept { return value.has_value(); }

    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    [[nodiscard]] const std::string& operator*() const noexcept { return *value; }

    [[nodiscard]] auto operator<=>(const PasteId&) const noexcept = default;
};

}  // namespace pastebin

template <>
struct glz::meta<pastebin::PasteId> {
    using T = pastebin::PasteId;
    static constexpr auto value = &T::value;
};
```

`ListPastes`'s pagination cursor is the same `hasValue()`-capable opaque-string
shape (`IMPLEMENTATION.md` rule 3's protocol-scalars row: "pagination
cursors... a named opaque newtype per role... never a loose `std::string`"),
so it lives in the same file, following the identical pattern — this is two
different concrete types following one shape, not the same helper reused a
third time, so the promotion rule does not apply here:

```cpp
namespace pastebin {

struct PasteCursor {
    std::optional<std::string> value;

    constexpr PasteCursor() noexcept = default;
    explicit PasteCursor(std::string token) noexcept : value{std::move(token)} {}
    explicit PasteCursor(std::optional<std::string> payload) noexcept : value{std::move(payload)} {}

    [[nodiscard]] bool hasValue() const noexcept { return value.has_value(); }

    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    [[nodiscard]] const std::string& operator*() const noexcept { return *value; }

    [[nodiscard]] auto operator<=>(const PasteCursor&) const noexcept = default;
};

/// @brief Trivial, fieldless acknowledgement result for actions with nothing
///        else to return (`DeletePaste`, `ExpirePaste`).
struct Ack {};

}  // namespace pastebin

template <>
struct glz::meta<pastebin::PasteCursor> {
    using T = pastebin::PasteCursor;
    static constexpr auto value = &T::value;
};
```

**Verify the `glz::meta` specialization's exact shape against
`morph::forms::Multiline`'s** (`include/morph/forms/widget_hints.hpp:125-128`,
already confirmed to exist as `struct glz::meta<morph::forms::Multiline> {
... };` in this session's research) **before writing this** — copy that
one's exact member/pointer convention verbatim rather than the sketch above
if they differ (the sketch assumes `value` maps directly to the wire string,
matching `Timestamp`/`Ranged`'s own `value` member name, but the precise
glaze incantation needs verifying against a real, currently-compiling
specialization).

- [ ] **Step 3: Write `examples/pastebin/include/pastebin/core/errors.hpp`**

Follows `examples/bank/include/bank/core/errors.hpp`'s exact shape (one base,
several `using Base::Base;` leaves):

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdexcept>
#include <string>

namespace pastebin {

/// @brief Base of every pastebin-specific error a model throws.
struct PastebinError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

/// @brief No paste exists at the given id (never existed, deleted, or
///        already expired/burned).
struct NotFound : PastebinError {
    using PastebinError::PastebinError;
};

/// @brief The paste existed but its `expiresAt` has passed.
struct Expired : PastebinError {
    using PastebinError::PastebinError;
};

/// @brief The paste existed but its burn-after-reads budget was already
///        exhausted before this read.
struct Burned : PastebinError {
    using PastebinError::PastebinError;
};

/// @brief An action's `validate()` rejected its input.
struct ValidationError : PastebinError {
    using PastebinError::PastebinError;
};

/// @brief `CreatePaste`'s content exceeded the server's message-size bound.
struct TooLarge : PastebinError {
    using PastebinError::PastebinError;
};

}  // namespace pastebin
```

- [ ] **Step 4: Commit**

```bash
git add examples/pastebin/include/pastebin/units.hpp \
        examples/pastebin/include/pastebin/core/types.hpp \
        examples/pastebin/include/pastebin/core/errors.hpp
git commit -m "pastebin: add unit system, PasteId, and the typed error set"
```

(This task produces headers only — nothing compiles into a target yet;
Task 8's CMake wiring is what first builds them. Verify with a standalone
`g++ -std=c++23 -fsyntax-only -I include -I <morph include> <file>` style
check, or defer syntax verification to Task 8's first real build — note in
the task report which approach was used.)

---

## Task 3: Pastebin DTOs

**Files:**
- Create: `examples/pastebin/include/pastebin/dto/paste_dto.hpp`

**Interfaces:**
- Consumes: `pastebin::PasteId`, `pastebin::PasteCursor`, `pastebin::Ack`
  (Task 2's `core/types.hpp`), `pastebin::Reads` (Task 2's `units.hpp`),
  `::morph::time::Timestamp` (`morph/util/datetime.hpp`).
- Produces: `CreatePaste`/`CreatePasteResult`, `GetPaste`/`PasteView`,
  `EditPaste` (result: `PasteView`), `DeletePaste`/`Ack`,
  `ListPastes`/`ListPastesResult`, `ExpirePaste`/`Ack`, `Visibility`,
  `Editability`, `PasteSummary`. Task 4 (entity) and Task 5 (model) consume
  every field name below verbatim.

Field set modeled on MicroBin's `Pasta` (id, content, extension, private,
editable, created, expiration, last_read, read_count, burn_after_reads),
translated through the strong-type rule — no `int`/`bool`/raw enum anywhere.
`editable`/`isPrivate` each become a two-enumerator `enum class`
(`IMPLEMENTATION.md` rule 3: "a two-state flag is a two-enumerator `enum
class`"), not `bool`.

- [ ] **Step 1: Write `examples/pastebin/include/pastebin/dto/paste_dto.hpp`**

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "pastebin/core/types.hpp"
#include "pastebin/units.hpp"

#include <morph/util/datetime.hpp>

#include <string>
#include <vector>

/// @file
/// Pastebin's one entity's wire DTOs. GetPaste is the one client-visible,
/// journaled mutation (README "Journal" design decision — not split into an
/// unlogged read + RecordRead). ExpirePaste is dispatched only by the
/// app-layer sweep's internal client (Task 6), never by a GUI client.

namespace pastebin {

enum class Visibility { Public, Private };
enum class Editability { Immutable, Editable };

struct CreatePaste {
    std::string content;
    std::string syntax;                  // free-form label, e.g. "plaintext", "cpp"
    ::morph::time::Timestamp expiresAt;   // empty = never expires
    Reads burnAfterReads;                 // empty = no burn limit
    Visibility visibility = Visibility::Public;
    Editability editability = Editability::Immutable;

    [[nodiscard]] bool validate() const noexcept { return !content.empty() && !syntax.empty(); }
};

struct CreatePasteResult {
    PasteId id;
};

struct GetPaste {
    PasteId id;

    [[nodiscard]] bool validate() const noexcept { return id.hasValue(); }
};

struct PasteView {
    PasteId id;
    std::string content;
    std::string syntax;
    ::morph::time::Timestamp createdAt;
    ::morph::time::Timestamp expiresAt;
    Reads burnAfterReads;
    Reads readCount;
    Visibility visibility = Visibility::Public;
    Editability editability = Editability::Immutable;
};

struct EditPaste {
    PasteId id;
    std::string content;
    std::string syntax;

    [[nodiscard]] bool validate() const noexcept { return id.hasValue() && !content.empty() && !syntax.empty(); }
};

struct DeletePaste {
    PasteId id;

    [[nodiscard]] bool validate() const noexcept { return id.hasValue(); }
};

/// @brief One row of `ListPastes`' result — deliberately narrower than
///        `PasteView`: a listing must not leak full paste content.
struct PasteSummary {
    PasteId id;
    std::string syntax;
    ::morph::time::Timestamp createdAt;
    Visibility visibility = Visibility::Public;
};

struct ListPastes {
    PasteCursor cursor;  // empty = first page
};

struct ListPastesResult {
    std::vector<PasteSummary> pastes;
    PasteCursor nextCursor;  // empty = no further page
};

/// @brief Internal-only: dispatched exclusively by the app-layer expiry
///        sweep's internal client (Task 6), never by a GUI client. Payload
///        is just the id — never `now()` — so replaying this entry is
///        trivially deterministic (README "How does expiry replay?").
struct ExpirePaste {
    PasteId id;

    [[nodiscard]] bool validate() const noexcept { return id.hasValue(); }
};

}  // namespace pastebin
```

- [ ] **Step 2: Commit**

```bash
git add examples/pastebin/include/pastebin/dto/paste_dto.hpp
git commit -m "pastebin: add the PasteModel action/result DTOs"
```

---

## Task 4: Pastebin entity and migration

**Files:**
- Create: `examples/pastebin/include/pastebin/db/paste_entity.hpp`
- Create: `examples/pastebin/src/db/schema.cpp`
- Create: `examples/pastebin/include/pastebin/db/database.hpp`
- Create: `examples/pastebin/include/pastebin/db/db_model.hpp`

**Interfaces:**
- Produces: `pastebin::db::PasteRecord` (Lightweight entity), one
  `LIGHTWEIGHT_SQL_MIGRATION` creating its table, `pastebin::db::setup(const
  std::string& connectionString)` (bootstrap, mirrors
  `bank::db::setup` — sets the default connection string, applies pending
  migrations). Task 5 (model) and Task 9 (tests, via `DbFixture`) consume
  `PasteRecord` and this migration directly.

Timestamps are stored as epoch-millisecond `Field<std::int64_t>`/
`Field<std::optional<std::int64_t>>` columns, matching every existing bank
entity's timestamp convention (`notification_entity.hpp`'s `createdAtMs`,
etc. — bank predates the strong-type *DTO* rule but its *storage*
convention for time is still the one worth reusing; no existing entity
stores a `Timestamp`/`DateTime` column directly, so this is the plan's own
choice, not a copied precedent). The model (Task 5) converts
`::morph::time::Timestamp` ⇄ epoch-millis explicitly at the DTO⇄entity
boundary.

- [ ] **Step 1: Write `examples/pastebin/include/pastebin/db/paste_entity.hpp`**

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Lightweight/DataMapper/DataMapper.hpp>

#include <cstdint>
#include <optional>
#include <string>

/// @file
/// PasteRecord: the one Lightweight entity this rung needs, kept strictly
/// separate from the wire DTOs (pastebin/dto/paste_dto.hpp) per
/// IMPLEMENTATION.md rule 4's two-type-layer architecture. `id` is the
/// animal-name key itself (the primary key IS the public id — no separate
/// surrogate integer key), so it is a plain string primary key, not
/// AutoIncrement.

namespace pastebin::db {

struct PasteRecord {
    static constexpr std::string_view TableName = "pastes";

    Light::Field<Light::SqlAnsiString<32>, Light::PrimaryKey::ManualAssign, Light::SqlRealName{"id"}> id;
    Light::Field<std::string, Light::SqlRealName{"content"}> content;
    Light::Field<Light::SqlAnsiString<32>, Light::SqlRealName{"syntax"}> syntax;
    Light::Field<std::int64_t, Light::SqlRealName{"created_at_ms"}> createdAtMs{0};
    Light::Field<std::optional<std::int64_t>, Light::SqlRealName{"expires_at_ms"}> expiresAtMs;
    Light::Field<std::optional<std::int64_t>, Light::SqlRealName{"burn_after_reads"}> burnAfterReads;
    Light::Field<std::int64_t, Light::SqlRealName{"read_count"}> readCount{0};
    Light::Field<bool, Light::SqlRealName{"is_private"}> isPrivate{false};
    Light::Field<bool, Light::SqlRealName{"is_editable"}> isEditable{false};
};

}  // namespace pastebin::db
```

**Verify `Light::PrimaryKey::ManualAssign` is the real enumerator name for
"caller supplies the primary key value, no auto-increment"** — confirmed by
its documented purpose but re-check the exact spelling against
`Lightweight/DataMapper/Field.hpp`'s `PrimaryKey` enum before writing this;
`bank`'s entities all use `PrimaryKey::AutoAssign`/
`ServerSideAutoIncrement` (surrogate integer keys), so this is pastebin's
first manually-assigned string primary key in this codebase — no existing
usage to copy verbatim.

- [ ] **Step 2: Write the migration in `examples/pastebin/src/db/schema.cpp`**

```cpp
// SPDX-License-Identifier: Apache-2.0
#include "pastebin/db/database.hpp"

#include <Lightweight/SqlConnection.hpp>
#include <Lightweight/SqlMigration.hpp>
#include <Lightweight/SqlQuery/Migrate.hpp>

using namespace Lightweight::SqlColumnTypeDefinitions;

LIGHTWEIGHT_SQL_MIGRATION(20260806000001, "Create pastes table") {
    plan.CreateTableIfNotExists("pastes")
        .PrimaryKey("id", Varchar(32))
        .RequiredColumn("content", Text())
        .RequiredColumn("syntax", Varchar(32))
        .RequiredColumn("created_at_ms", Bigint())
        .Column("expires_at_ms", Bigint())
        .Column("burn_after_reads", Bigint())
        .RequiredColumn("read_count", Bigint())
        .RequiredColumn("is_private", Bool())
        .RequiredColumn("is_editable", Bool());
}

namespace pastebin::db {

void setup(const std::string& connectionString) {
    Lightweight::SqlConnection::SetDefaultConnectionString(Lightweight::SqlConnectionString{connectionString});
    Lightweight::SqlMigration::MigrationManager::GetInstance().CreateMigrationHistory();
    Lightweight::SqlMigration::MigrationManager::GetInstance().ApplyPendingMigrations();
}

}  // namespace pastebin::db
```

**Verify `SqlCreateTableQueryBuilder`'s manual-primary-key method name**
(sketched above as `.PrimaryKey("id", Varchar(32))`, by analogy with
`.PrimaryKeyWithAutoIncrement(...)`'s naming) **against
`Lightweight/SqlQuery/Migrate.hpp` before writing this** — that file was
read in this session only for its `Column`/`RequiredColumn`/`RequiredForeignKey`
methods (confirmed real), not for a non-auto-increment primary-key method;
its exact name is not yet confirmed. Also verify `Text()`/`Bool()`/`Bigint()`
exist in `Lightweight::SqlColumnTypeDefinitions` alongside the
already-confirmed `Varchar{N}` (bank's migrations use `Varchar`/`Bigint`
already; `Text`/`Bool` are inferred from SQL column-type convention, not
independently confirmed this session).

- [ ] **Step 3: Write `examples/pastebin/include/pastebin/db/database.hpp`**

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>

/// @file
/// pastebin::db::setup — mirrors bank::db::setup's bootstrap shape
/// (examples/bank/include/bank/db/database.hpp): set the default connection
/// string, then apply every pending LIGHTWEIGHT_SQL_MIGRATION. The
/// migration itself lives in schema.cpp so linking that one TU registers it
/// against MigrationManager's process-wide singleton at static-init time.

namespace pastebin::db {

/// @brief Points Lightweight's default connection at @p connectionString and
///        applies every pending migration.
/// @param connectionString ODBC connection string (SQLite via sqliteodbc in
///        every ladder test/demo context).
void setup(const std::string& connectionString);

}  // namespace pastebin::db
```

- [ ] **Step 4: Commit**

```bash
git add examples/pastebin/include/pastebin/db/paste_entity.hpp \
        examples/pastebin/include/pastebin/db/database.hpp \
        examples/pastebin/include/pastebin/db/db_model.hpp \
        examples/pastebin/src/db/schema.cpp
git commit -m "pastebin: add PasteRecord entity, its migration, and the WithMapper mixin"
```

Also write `examples/pastebin/include/pastebin/db/db_model.hpp` in this
task — the `WithMapper` mixin `IMPLEMENTATION.md` rule 4 mandates ("one
lazily-opened mapper per model via the `WithMapper` mixin pattern"), copied
from `examples/bank/include/bank/db/db_model.hpp` (already read in full this
session, 27 lines) verbatim except the namespace (`pastebin::db` instead of
`bank::db`). Task 5's model inherits from it exactly as bank's models do.

**`pastebin::db::setup()` is production-bootstrap-only** (Task 6's server
app calls it once, at process start). Tests never call it: `DbFixture`
(rung 0's testkit) already sets the default connection string exactly once
per process and applies every pending migration on each fixture
construction — the `LIGHTWEIGHT_SQL_MIGRATION` this task registers is
picked up automatically the moment `ladder_pastebin_lib` is linked in,
`db::setup()` or not. Calling both in the same process would double-call
`SetDefaultConnectionString`, which is harmless but redundant — Task 9's
tests must not do it.

---

## Task 5: `PasteModel`

**Files:**
- Create: `examples/pastebin/include/pastebin/models/paste_model.hpp`
- Create: `examples/pastebin/src/models/paste_model.cpp`

**Interfaces:**
- Consumes: Task 2's `PasteId`/`PasteCursor`/`Ack`/`PastebinError` hierarchy,
  Task 3's DTOs, Task 4's `PasteRecord`/`db::WithMapper`, Task 1's
  `morph::ladder::now()`.
- Produces: `pastebin::PasteModel`, registered via
  `BRIDGE_REGISTER_MODEL`/`BRIDGE_REGISTER_ACTION` (plain, not shared/keyed —
  resolved design decision). Task 6 (app bootstrap/sweep), Task 9 (model
  tests), and Task 10 (presenters) all consume this exact registration.

This is the application (`IMPLEMENTATION.md` rule 1) — every business rule
lives here, nothing domain-shaped in the app bootstrap, presenters, or GUI.

### Step 1 (do this first): spike-verify `UPDATE ... RETURNING` against this codebase's toolchain

The README's resolved burn-atomicity design needs a single atomic
`UPDATE pastes SET read_count = read_count + 1 WHERE ... RETURNING ...`
issued through Lightweight's raw-query facility
(`Lightweight::SqlStatement::Prepare`/`Execute`/`FetchRow`/`GetColumn<T>` —
the shape `Lightweight/src/tests/CoreTests.cpp:202-234` demonstrates for an
ordinary parameterized statement). **No existing Lightweight test or
example anywhere in this codebase uses SQL `RETURNING`** — this exact
combination (Lightweight's raw-query path + the sqliteodbc driver this
repo's tests run against) is unverified. Before writing `execute(GetPaste)`
for real:

- [ ] **Step 1a: Write a standalone throwaway smoke** (in a scratch `.cpp`,
      or as the first thing tried directly in a `DbFixture`-backed Catch2
      `TEST_CASE` that will become part of Task 9's real test file) that:
      creates a tiny probe table, inserts one row, issues
      `UPDATE probe SET n = n + 1 WHERE id = ? RETURNING n` via
      `SqlStatement::Prepare`/`Execute`/`FetchRow`/`GetColumn<int>`, and
      asserts the returned `n` is the incremented value.
- [ ] **Step 1b: If it works** — proceed with the design below verbatim.
- [ ] **Step 1c: If it does not work** (a bind error, a syntax error from
      the SQLite ODBC driver, or `RETURNING` silently returning nothing) —
      do not spend more than one focused attempt debugging the driver
      combination itself. Fall back to the transaction-wrapped two-statement
      form instead: `Lightweight::SqlTransaction` wrapping (1) the plain
      conditional `UPDATE ... WHERE ...` (no `RETURNING`, checking
      `SqlStatement::Execute(...)`'s affected-row-count instead of a
      returned row) and (2) an ordinary `SELECT` by id to fetch the
      resulting row state, both against the same connection inside the one
      transaction — still atomic (SQLite serializes writers; the
      transaction keeps the read-back consistent with the write), just two
      statements instead of one. **Either way, update
      `examples/pastebin/README.md`'s burn-atomicity paragraph to say which
      form actually shipped**, and file the mandatory finding (the README
      already names the trigger: "with its mandatory finding entry filed
      once the `RETURNING` combination... is verified") reporting exactly
      what was tried and what happened — a working `RETURNING` closes it
      as `documented-limitation` ("works, now proven"); a failing one is
      `open` with the concrete error captured.

### Step 2: Write `examples/pastebin/include/pastebin/models/paste_model.hpp`

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "pastebin/core/errors.hpp"
#include "pastebin/db/db_model.hpp"
#include "pastebin/dto/paste_dto.hpp"

#include <morph/core/bridge.hpp>

namespace pastebin {

/// @brief The one model this rung ships. Registered plain (no
///        BRIDGE_MODEL_KEY/AllowShared — README's resolved burn-atomicity
///        decision): every action dispatch gets a fresh instance, all real
///        state lives in `pastes` via `db::WithMapper`.
class PasteModel : public db::WithMapper {
  public:
    CreatePasteResult execute(CreatePaste action);
    PasteView execute(GetPaste action);
    PasteView execute(EditPaste action);
    Ack execute(DeletePaste action);
    ListPastesResult execute(ListPastes action);

    /// @brief Dispatched only by the app-layer expiry sweep's internal
    ///        client (Task 6) — never by a GUI client.
    Ack execute(ExpirePaste action);
};

}  // namespace pastebin

BRIDGE_REGISTER_MODEL(pastebin::PasteModel, "PasteModel")
BRIDGE_REGISTER_ACTION(pastebin::PasteModel, pastebin::CreatePaste, "CreatePaste")
// GetPaste stays the one client-visible, journaled action (default
// Loggable::Yes) — README's resolved journal decision; do not add
// ::morph::model::Loggable::No here.
BRIDGE_REGISTER_ACTION(pastebin::PasteModel, pastebin::GetPaste, "GetPaste")
BRIDGE_REGISTER_ACTION(pastebin::PasteModel, pastebin::EditPaste, "EditPaste")
BRIDGE_REGISTER_ACTION(pastebin::PasteModel, pastebin::DeletePaste, "DeletePaste")
BRIDGE_REGISTER_ACTION(pastebin::PasteModel, pastebin::ListPastes, "ListPastes", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(pastebin::PasteModel, pastebin::ExpirePaste, "ExpirePaste")
```

**Verify the exact `BRIDGE_REGISTER_MODEL`/`BRIDGE_REGISTER_ACTION` macro
argument order and the `::morph::model::Loggable` enum's namespace/spelling**
against `examples/bank/include/bank/models/notification_model.hpp:33-37`
(already read in full this session) before writing this — copy that file's
macro invocations' exact shape, substituting only the type/string names
above.

### Step 3: Write `examples/pastebin/src/models/paste_model.cpp`

```cpp
// SPDX-License-Identifier: Apache-2.0
#include "pastebin/models/paste_model.hpp"

#include "common/clock.hpp"

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <Lightweight/SqlStatement.hpp>

#include <algorithm>
#include <array>
#include <random>
#include <string>

namespace pastebin {

namespace {

// ---------------------------------------------------------------------------
// DTO <-> entity conversions (IMPLEMENTATION.md rule 4's DTO<->entity mapping
// layer). Timestamp <-> epoch-ms and Reads <-> int64 both round-trip through
// a plain scalar since every value either DTO type carries is, by
// construction, a whole number of milliseconds / a whole-number count.
// ---------------------------------------------------------------------------

[[nodiscard]] std::int64_t toEpochMs(const ::morph::time::DateTime& instant) noexcept {
    return instant.value.time_since_epoch().count();
}

[[nodiscard]] ::morph::time::Timestamp fromEpochMs(std::optional<std::int64_t> ms) noexcept {
    if (!ms) {
        return ::morph::time::Timestamp{};
    }
    return ::morph::time::Timestamp{::morph::time::DateTime{
        std::chrono::sys_time<std::chrono::milliseconds>{std::chrono::milliseconds{*ms}}}};
}

/// @brief Builds the read-only view sent back to a client from a fully
///        loaded `PasteRecord`.
[[nodiscard]] PasteView toView(const db::PasteRecord& rec) {
    PasteView view;
    view.id = PasteId{rec.id.Value().AsStringView() | std::ranges::to<std::string>()};
    view.content = rec.content.Value();
    view.syntax = rec.syntax.Value().AsStringView() | std::ranges::to<std::string>();
    view.createdAt = fromEpochMs(rec.createdAtMs.Value());
    view.expiresAt = fromEpochMs(rec.expiresAtMs.Value());
    view.burnAfterReads = rec.burnAfterReads.Value() ? Reads::fromDouble(static_cast<double>(*rec.burnAfterReads.Value())) : Reads{};
    view.readCount = Reads::fromDouble(static_cast<double>(rec.readCount.Value()));
    view.visibility = rec.isPrivate.Value() ? Visibility::Private : Visibility::Public;
    view.editability = rec.isEditable.Value() ? Editability::Editable : Editability::Immutable;
    return view;
}

/// @brief The tiny animal-name id keyspace (MicroBin-style). Deliberately
///        small — the required tests exercise the id-collision retry path,
///        which needs collisions to be reachable in a bounded number of
///        CreatePaste calls, not astronomically unlikely.
constexpr std::array<std::string_view, 16> kAnimals = {
    "cat",    "dog",   "fox",    "owl",  "bee",   "ant",   "elk",  "ram",
    "yak",    "cod",   "eel",    "hen",  "pig",   "cow",   "bat",  "jay",
};
constexpr std::array<std::string_view, 16> kAdjectives = {
    "red",    "blue",  "gold",   "dark", "swift", "calm",  "bold", "wild",
    "keen",   "grey",  "warm",   "cool", "sharp", "quiet", "loud", "soft",
};

[[nodiscard]] std::string randomPasteId() {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    std::uniform_int_distribution<std::size_t> adjIdx{0, kAdjectives.size() - 1};
    std::uniform_int_distribution<std::size_t> animalIdx{0, kAnimals.size() - 1};
    std::uniform_int_distribution<int> suffix{0, 999};
    return std::string{kAdjectives[adjIdx(rng)]} + "-" + std::string{kAnimals[animalIdx(rng)]} + "-" +
           std::to_string(suffix(rng));
}

}  // namespace

CreatePasteResult PasteModel::execute(CreatePaste action) {
    if (!action.validate()) {
        throw ValidationError{"CreatePaste: content and syntax are required"};
    }

    // Bounded retry on the (small, deliberately-collidable) animal-name
    // keyspace — the "id-collision handling" required test drives this
    // path directly by exhausting the space or by pre-seeding a collision.
    constexpr int kMaxAttempts = 8;
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        db::PasteRecord rec;
        rec.id = randomPasteId();
        rec.content = action.content;
        rec.syntax = action.syntax;
        rec.createdAtMs = toEpochMs(*morph::ladder::now().value);
        rec.expiresAtMs = action.expiresAt.hasValue() ? std::optional{toEpochMs(*action.expiresAt.value)} : std::nullopt;
        rec.burnAfterReads = action.burnAfterReads.hasValue()
                                  ? std::optional{static_cast<std::int64_t>(action.burnAfterReads.value()->toDouble())}
                                  : std::nullopt;
        rec.readCount = 0;
        rec.isPrivate = action.visibility == Visibility::Private;
        rec.isEditable = action.editability == Editability::Editable;

        try {
            mapper().Create(rec);
            return CreatePasteResult{.id = PasteId{*rec.id.Value().AsStringView() | std::ranges::to<std::string>()}};
        } catch (const std::exception&) {
            // Primary-key collision on the animal-name id — retry with a
            // fresh random id. Lightweight surfaces a constraint violation
            // as a thrown exception (no narrower type to catch on
            // specifically at this layer); if kMaxAttempts is exhausted the
            // loop falls through and the function throws ValidationError
            // below, which is the caller-visible "keyspace exhausted"
            // signal (Required tests: "id-collision handling").
            continue;
        }
    }
    throw ValidationError{"CreatePaste: could not allocate a unique paste id"};
}

PasteView PasteModel::execute(GetPaste action) {
    if (!action.validate()) {
        throw ValidationError{"GetPaste: id is required"};
    }

    const std::int64_t nowMs = toEpochMs(*morph::ladder::now().value);

    ::Lightweight::SqlStatement stmt;
    stmt.Prepare(R"(UPDATE pastes
                     SET read_count = read_count + 1
                     WHERE id = ?
                       AND (expires_at_ms IS NULL OR expires_at_ms > ?)
                       AND (burn_after_reads IS NULL OR read_count < burn_after_reads)
                     RETURNING content, syntax, created_at_ms, expires_at_ms,
                               burn_after_reads, read_count, is_private, is_editable)");
    auto cursor = stmt.Execute(*action.id.value, nowMs);

    if (cursor.FetchRow()) {
        PasteView view;
        view.id = action.id;
        view.content = cursor.GetColumn<std::string>(1);
        view.syntax = cursor.GetColumn<std::string>(2);
        view.createdAt = fromEpochMs(cursor.GetColumn<std::int64_t>(3));
        view.expiresAt = fromEpochMs(cursor.GetColumn<std::optional<std::int64_t>>(4));
        const auto burnAfter = cursor.GetColumn<std::optional<std::int64_t>>(5);
        const auto readCount = cursor.GetColumn<std::int64_t>(6);
        view.burnAfterReads = burnAfter ? Reads::fromDouble(static_cast<double>(*burnAfter)) : Reads{};
        view.readCount = Reads::fromDouble(static_cast<double>(readCount));
        view.visibility = cursor.GetColumn<bool>(7) ? Visibility::Private : Visibility::Public;
        view.editability = cursor.GetColumn<bool>(8) ? Editability::Editable : Editability::Immutable;

        // The read that just consumed the last allowed budget deletes the
        // paste after building its result — burn-after-read's "delete on
        // the Nth read, not before" semantics.
        if (burnAfter && readCount >= *burnAfter) {
            ::Lightweight::SqlStatement del;
            del.Prepare("DELETE FROM pastes WHERE id = ?");
            del.Execute(*action.id.value);
        }
        return view;
    }

    // The atomic update matched zero rows — classify why via a plain,
    // unprotected read. This does not reopen the race the atomic update
    // closed: it only decides *which* error to throw, it performs no
    // mutation.
    auto existing = mapper().Query<db::PasteRecord>().Where(Lightweight::FieldNameOf<&db::PasteRecord::id>, "=", *action.id.value).All();
    if (existing.empty()) {
        throw NotFound{"GetPaste: no such paste"};
    }
    const auto& row = existing.front();
    if (row.expiresAtMs.Value() && *row.expiresAtMs.Value() <= nowMs) {
        throw Expired{"GetPaste: paste has expired"};
    }
    if (row.burnAfterReads.Value() && row.readCount.Value() >= *row.burnAfterReads.Value()) {
        throw Burned{"GetPaste: paste's burn-after-reads budget is exhausted"};
    }
    throw NotFound{"GetPaste: no such paste"};
}

PasteView PasteModel::execute(EditPaste action) {
    if (!action.validate()) {
        throw ValidationError{"EditPaste: id, content, and syntax are required"};
    }
    auto rows = mapper().Query<db::PasteRecord>().Where(Lightweight::FieldNameOf<&db::PasteRecord::id>, "=", *action.id.value).All();
    if (rows.empty()) {
        throw NotFound{"EditPaste: no such paste"};
    }
    auto rec = rows.front();
    if (!rec.isEditable.Value()) {
        throw ValidationError{"EditPaste: paste is not editable"};
    }
    rec.content = action.content;
    rec.syntax = action.syntax;
    mapper().Update(rec);
    return toView(rec);
}

Ack PasteModel::execute(DeletePaste action) {
    if (!action.validate()) {
        throw ValidationError{"DeletePaste: id is required"};
    }
    ::Lightweight::SqlStatement stmt;
    stmt.Prepare("DELETE FROM pastes WHERE id = ?");
    stmt.Execute(*action.id.value);
    return Ack{};
}

ListPastesResult PasteModel::execute(ListPastes action) {
    constexpr int kPageSize = 20;
    auto query = mapper().Query<db::PasteRecord>().Where(Lightweight::FieldNameOf<&db::PasteRecord::isPrivate>, "=", false);
    if (action.cursor.hasValue()) {
        query = query.Where(Lightweight::FieldNameOf<&db::PasteRecord::id>, "<", *action.cursor.value);
    }
    auto rows = query.OrderBy(Lightweight::FieldNameOf<&db::PasteRecord::id>, Lightweight::SqlResultOrdering::DESCENDING)
                    .Limit(kPageSize + 1)
                    .All();

    ListPastesResult result;
    const bool hasMore = rows.size() > kPageSize;
    if (hasMore) {
        rows.resize(kPageSize);
    }
    for (const auto& row : rows) {
        result.pastes.push_back(PasteSummary{
            .id = PasteId{*row.id.Value().AsStringView() | std::ranges::to<std::string>()},
            .syntax = *row.syntax.Value().AsStringView() | std::ranges::to<std::string>(),
            .createdAt = fromEpochMs(row.createdAtMs.Value()),
            .visibility = row.isPrivate.Value() ? Visibility::Private : Visibility::Public,
        });
    }
    result.nextCursor = hasMore ? PasteCursor{*rows.back().id.Value().AsStringView() | std::ranges::to<std::string>()} : PasteCursor{};
    return result;
}

Ack PasteModel::execute(ExpirePaste action) {
    if (!action.validate()) {
        throw ValidationError{"ExpirePaste: id is required"};
    }
    ::Lightweight::SqlStatement stmt;
    stmt.Prepare("DELETE FROM pastes WHERE id = ? AND expires_at_ms IS NOT NULL AND expires_at_ms <= ?");
    stmt.Execute(*action.id.value, toEpochMs(*morph::ladder::now().value));
    return Ack{};
}

}  // namespace pastebin
```

**This is a sketch to transcribe against the real APIs, not blind
copy-paste** — several call shapes here are inferred from partially-verified
signatures and must be checked against the real headers while implementing:

- `Lightweight::SqlStatement::Execute(...)`'s exact parameter-binding and
  return-cursor API (verified shape from `CoreTests.cpp:202-234`: `Prepare`
  then `Execute(args...)` returns something `FetchRow()`/`GetColumn<T>(index)`
  work on — confirm the cursor type's real name and 1-based-vs-0-based
  column indexing against that test file directly).
- `Light::SqlAnsiString<N>::AsStringView()` and whether `Field<>::Value()`
  returns by value or reference, and whether a `std::optional<std::int64_t>`
  column really round-trips through `Field<std::optional<std::int64_t>>`
  exactly as sketched (confirmed the *type* compiles per
  `FieldTests.cpp:53-161`, not confirmed the exact accessor chain above).
- `Lightweight::DataMapper::Query<T>().Where(...).OrderBy(...).Limit(...).All()`'s
  exact chain — `Where(FieldNameOf<&T::field>, "op", value)` is confirmed
  (bank's `notification_model.cpp`); `OrderBy`/`Limit`/`SqlResultOrdering`
  are inferred by DataMapper-query-builder convention, not independently
  confirmed this session — check `Lightweight/DataMapper/QueryBuilders.hpp`
  for their real names before trusting the sketch.
- `Reads::fromDouble(double)` (confirmed to exist, per
  `include/morph/util/quantity.hpp`'s `Quantity` API) and
  `math::Rational::toDouble()` (used above to convert a stored `Reads`
  action field back to `int64_t` for the SQL bind) — the second is *not*
  independently confirmed; check `include/morph/math/rational.hpp` (or
  wherever `Rational` lives) for its real double-conversion accessor name
  before writing the `CreatePaste`/`toView` conversions.
- Every `throw ValidationError{"..."}` etc. call needs `PastebinError`'s
  constructor to accept a string literal directly (it inherits
  `std::runtime_error`'s constructors via `using Base::Base;`, confirmed in
  Task 2 — this one is solid).

### Step 4: Compile-check and adjust

```bash
cmake --build build/<preset> --target ladder_pastebin_lib
```

Expect real compile errors on the inferred APIs flagged above — this is
the normal, expected outcome of transcribing a sketch against real headers,
not a plan defect. Fix forward against the real signatures; do not
introduce a mock/shim layer to paper over an API mismatch.

### Step 5: Commit

```bash
git add examples/pastebin/include/pastebin/models/paste_model.hpp \
        examples/pastebin/src/models/paste_model.cpp
git commit -m "pastebin: add PasteModel (create/get/edit/delete/list/expire)"
```

Model tests are Task 9, deliberately deferred until Task 6 (the app
bootstrap + expiry sweep, which `ExpirePaste`'s only real caller lives in)
and Task 7 (the `db_fault_fixture` extension the store-error tests need)
both exist — this task's own review should still build and manually smoke
`CreatePaste`/`GetPaste` round-trips (e.g. a scratch `main()` or an
early, throwaway Catch2 case later folded into Task 9's real file) before
moving on, per this plan's TDD spirit, even though the durable test file
lands in Task 9.

---

## Task 6: App bootstrap — `RemoteServer`, `FileActionLog`, the periodic expiry sweep

**Files:**
- Create: `examples/pastebin/include/pastebin/app/app.hpp`
- Create: `examples/pastebin/src/app/app.cpp`

**Interfaces:**
- Consumes: Task 5's `PasteModel`/`ExpirePaste`, Task 4's `PasteRecord`/
  `db::setup`, Task 1's `morph::ladder::now()`.
- Produces: `pastebin::app::App` — owns the worker pool, the
  `RemoteServer` every real transport (a `QtWebSocketServer`, Task 12's
  server binary) or `BackendRig` test wraps, the installed
  `FileActionLog`, and the periodic expiry sweep. Task 9 (tests), Task 12
  (server binary), and Task 13 (final CI wiring) all construct one.

`App` is intentionally **not** Qt-Core-only (unlike `gui_lib`,
`TESTING.md`'s presenter rule 1 constraint) — it is server-side
orchestration, not a presenter, and it needs `QTimer` for the sweep. It
does not itself construct a `QtWebSocketServer`: that stays the caller's
job (Task 12's server binary wraps `App::server()` in one; `BackendRig`
tests never need to).

- [ ] **Step 1: Write `examples/pastebin/include/pastebin/app/app.hpp`**

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/remote.hpp>
#include <morph/journal/file_action_log.hpp>
#include <morph/qt/qt_executor.hpp>

#include <QObject>
#include <QTimer>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>

namespace pastebin::app {

/// @brief Owns the server-side pieces every pastebin deployment shares: the
/// worker pool, the `RemoteServer`, the durable `FileActionLog` (installed
/// process-wide via `morph::journal::setActionLog`, so every `PasteModel`
/// instance auto-attaches — see its own doc comment), and the periodic
/// expiry sweep. Nothing here decides deployment mode (`Local`/`Remote`) —
/// that stays `examples/common/gui::AppContext`'s job on the client side;
/// this is exclusively the server side.
///
/// The expiry sweep dispatches `ExpirePaste{id}` through an **internal
/// client** — a `Bridge` over `SimulatedRemoteBackend{*server()}` — a
/// first-class client of the same `RemoteServer` a real socket client
/// talks to (`SimulatedRemoteBackend::execute()` calls
/// `RemoteServer::handle()`, the identical dispatch path), so every swept
/// expiry is authorized, dispatched, and auto-journaled exactly like a
/// client-issued action. See `examples/pastebin/README.md`'s "How does
/// expiry replay?" for the full rationale, including why sweep *timing*
/// does not affect correctness (`PasteModel::execute(GetPaste)`'s own
/// atomic update already excludes an expired row on its own).
class App : public QObject {
    Q_OBJECT
  public:
    /// @param actionLogPath Where `FileActionLog` persists entries.
    /// @param sweepInterval How often the expiry sweep runs. Tests pass a
    ///        long interval (effectively disabling the timer) and call
    ///        `sweepExpiredOnce()` directly instead, for determinism.
    /// @param workers        Size of the model worker pool.
    /// @param parent         Optional `QObject` parent.
    explicit App(std::filesystem::path actionLogPath, std::chrono::milliseconds sweepInterval = std::chrono::seconds{5},
                 std::size_t workers = 4, QObject* parent = nullptr);

    /// @brief Detaches the process-wide default action log.
    ~App() override;

    App(const App&) = delete;
    App& operator=(const App&) = delete;
    App(App&&) = delete;
    App& operator=(App&&) = delete;

    /// @brief The server every transport (a `QtWebSocketServer`, a test's
    ///        `BackendRig`) wraps or dispatches against.
    [[nodiscard]] std::shared_ptr<::morph::backend::RemoteServer> server() const noexcept { return _server; }

    /// @brief Runs one expiry sweep pass right now: finds every paste whose
    ///        `expires_at_ms` has passed and fire-and-forget dispatches
    ///        `ExpirePaste` for each through the internal client. Does not
    ///        block on the dispatched calls settling — callers that need
    ///        to observe completion (tests) pump the Qt event loop
    ///        afterward (`morph::ladder::testkit::pumpUntil`).
    void sweepExpiredOnce();

  private:
    ::morph::exec::ThreadPoolExecutor _pool;
    std::shared_ptr<::morph::journal::FileActionLog> _actionLog;
    std::shared_ptr<::morph::backend::RemoteServer> _server;
    ::morph::qt::QtExecutor _sweepExecutor;
    ::morph::bridge::Bridge _sweepBridge;
    QTimer _sweepTimer;
};

}  // namespace pastebin::app
```

**Verify `RemoteServer`'s real constructor signature**
(`explicit RemoteServer(exec::IExecutor& workerPool, ...)`, per this
session's earlier research — confirm the exact parameter list, including
whether it takes the pool by reference or the `ThreadPoolExecutor`
directly, against `include/morph/core/remote.hpp` before writing the
member-initializer list in Step 2) and **`Bridge`'s constructor** (takes
`std::unique_ptr<IBackend>`, confirmed this session) before writing
`_sweepBridge`'s initializer — `_sweepBridge` must be constructed with a
`SimulatedRemoteBackend` wrapping `*_server`, which itself must already
exist (`_server` is declared before `_sweepBridge` in the member list
above deliberately, so member-initialization order — which follows
declaration order, not initializer-list order — constructs `_server`
first).

- [ ] **Step 2: Write `examples/pastebin/src/app/app.cpp`**

```cpp
// SPDX-License-Identifier: Apache-2.0
#include "pastebin/app/app.hpp"

#include "common/clock.hpp"
#include "pastebin/db/paste_entity.hpp"
#include "pastebin/dto/paste_dto.hpp"
#include "pastebin/models/paste_model.hpp"

#include <morph/core/logger.hpp>

#include <Lightweight/SqlStatement.hpp>

namespace pastebin::app {

App::App(std::filesystem::path actionLogPath, std::chrono::milliseconds sweepInterval, std::size_t workers,
         QObject* parent)
    : QObject{parent},
      _pool{workers},
      _actionLog{std::make_shared<::morph::journal::FileActionLog>(std::move(actionLogPath))},
      _server{std::make_shared<::morph::backend::RemoteServer>(_pool)},
      _sweepBridge{std::make_unique<::morph::backend::SimulatedRemoteBackend>(*_server)} {
    ::morph::journal::setActionLog(_actionLog);
    connect(&_sweepTimer, &QTimer::timeout, this, &App::sweepExpiredOnce);
    _sweepTimer.start(sweepInterval);
}

App::~App() {
    ::morph::journal::setActionLog(nullptr);
}

void App::sweepExpiredOnce() {
    const std::int64_t nowMs = morph::ladder::now().value->value.time_since_epoch().count();

    std::vector<std::string> expiredIds;
    {
        ::Lightweight::SqlStatement stmt;
        stmt.Prepare("SELECT id FROM pastes WHERE expires_at_ms IS NOT NULL AND expires_at_ms <= ?");
        auto cursor = stmt.Execute(nowMs);
        while (cursor.FetchRow()) {
            expiredIds.push_back(cursor.GetColumn<std::string>(1));
        }
    }

    ::morph::bridge::BridgeHandler<PasteModel> handler{_sweepBridge, &_sweepExecutor};
    for (const auto& id : expiredIds) {
        handler.execute(ExpirePaste{.id = PasteId{id}})
            .then([](Ack) {})
            .onError([id](const std::exception_ptr&) {
                ::morph::log::logError("[pastebin::App] expiry sweep: ExpirePaste failed for " + id);
            });
    }
}

}  // namespace pastebin::app
```

**Verify every inferred piece before trusting this sketch**: `RemoteServer`'s
constructor taking `_pool` directly (vs. needing `&_pool` or a different
argument shape — confirmed pattern from bank:
`std::make_shared<RemoteServer>(serverPool, ...)` where `serverPool` is a
`ThreadPoolExecutor` by value-reference, matching the sketch, but re-check);
`SimulatedRemoteBackend`'s constructor (confirmed: `explicit
SimulatedRemoteBackend(RemoteServer&)`); `BridgeHandler<Model>`'s
constructor taking `(Bridge&, IExecutor*)` (confirmed, used throughout this
codebase); `morph::log::logError`'s real signature (a `std::string` overload
is assumed — check `include/morph/core/logger.hpp`).

- [ ] **Step 3: Build**

```bash
cmake --build build/<preset> --target ladder_pastebin_lib
```

- [ ] **Step 4: Commit**

```bash
git add examples/pastebin/include/pastebin/app/app.hpp \
        examples/pastebin/src/app/app.cpp
git commit -m "pastebin: add App (RemoteServer bootstrap, FileActionLog, expiry sweep)"
```

`App`'s own tests are folded into Task 9 (the sweep is exercised through
`PasteModel`'s expiry-edge test cases, not a standalone `test_app.cpp` —
`App` has no behavior of its own worth testing in isolation from the model
it drives).

---

## Task 7: Extend `db_fault_fixture` — resolve finding 018 for this rung

**Files:**
- Create: `examples/common/testkit/db_busy_fixture.hpp`
- Create: `examples/common/testkit/test_db_busy_fixture.cpp`
- Modify: `examples/common/CMakeLists.txt` (add the new test file)
- Modify: `examples/pastebin/README.md` (mark finding 018 resolved for this
  rung's actual store-error tests, once Task 9 uses this)

**Interfaces:**
- Produces: `morph::ladder::testkit::DbBusyFixture` — forces a genuine
  `SQLITE_BUSY` on the *shared test database* by holding an uncommitted
  write transaction open on a second `SqlConnection` for the fixture's
  lifetime. Task 9's store-error tests are the first real consumer.

Per finding 018's own disposition ("real failures through the schema... a
competing write transaction to force a genuine `SQLITE_BUSY`"), this is a
**new, additional** fixture alongside `DbFaultFixture`
(`db_fault_fixture.hpp`), not a replacement — `DbFaultFixture`'s
`SqlScopedLock`-based contention stays as-is for whatever already depends
on it. Two of the three failure classes finding 018 names need **no new
fixture at all**, and Task 9 exercises them with ordinary test setup, not
this task's output:

- **`UNIQUE` violation**: trivially reachable — a test inserts a row at an
  id `CreatePaste`'s retry loop will collide on, or (more directly) calls
  `mapper().Create(rec)` twice with the same `rec.id` and asserts the
  second throws. No fixture needed.
- **The atomic `RETURNING` update's zero-rows-affected branch**: reachable
  by seeding a row already at `read_count == burn_after_reads` (or past
  `expires_at_ms`) and calling `GetPaste` against it — exactly the
  `Burned`/`Expired`/`NotFound` classification branch `PasteModel::execute
  (GetPaste)` already has to have (Task 5). No fixture needed; this is
  ordinary model-test setup, already covered by Task 9's required "Expiry
  edges" test.

**`SQLITE_BUSY`** is the one genuinely needing new fixture support — an
ordinary `DataMapper` write only ever hits it under real write contention.

- [ ] **Step 1: Write `examples/common/testkit/db_busy_fixture.hpp`**

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "db_fixture.hpp"

#include <Lightweight/Lightweight.hpp>

#include <string>

/// @file
/// Resolves docs/findings/018 (db_fault_fixture cannot fault an ordinary
/// DataMapper call) for the SQLITE_BUSY failure class specifically: holds a
/// genuine, uncommitted write transaction open on a second SqlConnection to
/// the shared test database, for the fixture's lifetime, so a concurrent
/// write from the code under test's own connection (via mapper()'s default
/// connection) collides for real — no mock, no simulated driver.

namespace morph::ladder::testkit {

/// @brief Holds an open write transaction on @p tableName for its lifetime,
///        forcing a concurrent write from a different connection to that
///        same table to observe `SQLITE_BUSY` (subject to the writer's own
///        ODBC busy-timeout — see the class's usage note in the test file
///        this ships alongside).
class DbBusyFixture {
  public:
    /// @param tableName Table to lock — must already exist (construct this
    ///        fixture after a `DbFixture` has applied migrations).
    explicit DbBusyFixture(std::string tableName);

    ~DbBusyFixture();

    DbBusyFixture(const DbBusyFixture&) = delete;
    DbBusyFixture& operator=(const DbBusyFixture&) = delete;
    DbBusyFixture(DbBusyFixture&&) = delete;
    DbBusyFixture& operator=(DbBusyFixture&&) = delete;

  private:
    std::string _tableName;
    ::Lightweight::SqlConnection _lockingConnection;
};

}  // namespace morph::ladder::testkit
```

- [ ] **Step 2: Implement it — hold a real uncommitted write**

Inline in the header (matching this testkit's existing header-only
convention for its small fixtures) or a `.cpp` if the implementation needs
`SqlStatement`/`SqlTransaction` includes not otherwise pulled in — the
constructor should: open `_lockingConnection`, begin a transaction on it
(`Lightweight::SqlTransaction` or a raw `BEGIN IMMEDIATE` via
`SqlStatement::ExecuteDirect` — check which one gives SQLite's *write* lock
immediately rather than deferring it to the first actual write, since a
plain `BEGIN` defers locking until the first statement touches data;
`BEGIN IMMEDIATE` is the SQLite-specific way to force it up front — verify
Lightweight's `SqlTransaction` exposes this, or fall back to
`ExecuteDirect("BEGIN IMMEDIATE")` directly followed by a real `UPDATE`
against one row of `tableName`, e.g. `UPDATE <tableName> SET rowid = rowid
LIMIT 0` is not valid SQL for forcing a lock without changing data — use
`UPDATE <tableName> SET id = id` (a no-op value write that still takes the
write lock) if the table has an `id` column, which every ladder entity to
date does). The destructor rolls back (or simply lets the connection's own
destruction release the lock — verify `SqlConnection`'s destructor behavior
with an open, uncommitted transaction).

- [ ] **Step 3: Write `examples/common/testkit/test_db_busy_fixture.cpp`**

```cpp
// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include "testkit/db_busy_fixture.hpp"
#include "testkit/db_fixture.hpp"

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <Lightweight/SqlMigration.hpp>

namespace {

struct BusyProbe {
    static constexpr std::string_view TableName = "busy_fixture_probe";
    Lightweight::Field<uint64_t, Lightweight::PrimaryKey::AutoAssign> id;
    Lightweight::Field<std::string> label;
};

}  // namespace

LIGHTWEIGHT_SQL_MIGRATION(2, "busy_fixture_probe: create probe table") {
    plan.CreateTable("busy_fixture_probe")
        .PrimaryKeyWithAutoIncrement("id")
        .Column("label", Lightweight::SqlColumnTypeDefinitions::Varchar{64});
}

TEST_CASE("DbBusyFixture forces a genuine SQLITE_BUSY on a concurrent write to the same table",
          "[ladder][testkit][db][busy]") {
    morph::ladder::testkit::DbFixture fixture;
    {
        Lightweight::DataMapper mapper;
        BusyProbe row;
        row.label = "seed";
        mapper.Create(row);
    }

    morph::ladder::testkit::DbBusyFixture busy{"busy_fixture_probe"};

    Lightweight::DataMapper mapper;
    BusyProbe row;
    row.label = "should collide";
    REQUIRE_THROWS(mapper.Create(row));
}
```

**Verify the exact exception type/message a genuine `SQLITE_BUSY` surfaces
as through Lightweight** (a generic `std::runtime_error` is the safe
`REQUIRE_THROWS` bet above; tighten to a narrower assertion — e.g. matching
`"SQLITE_BUSY"`/`"database is locked"` in the message — once the real text
is observed from a passing run, so this test cannot silently degrade into
"throws for any reason").

**If `BEGIN IMMEDIATE` + a no-op `UPDATE` does not reliably force the lock
within a bounded wait** (SQLite/ODBC driver timing can be finicky here —
this is genuinely unverified in this codebase, like Task 5's `RETURNING`
spike): shorten the busy-timeout the *test's own* connection uses via
`ODBC_CONNECTION_STRING`/`DbFixture::computeConnectionString`'s existing
override (e.g. `Timeout=200` instead of the default `5000`) so a failing
attempt surfaces in milliseconds instead of the full 5s default, and
document whatever the real, working recipe turns out to be directly in this
fixture's doc comment — do not leave the sketch above unverified in the
shipped file.

- [ ] **Step 4: Add the new test file to `examples/common/CMakeLists.txt`**

Same `ladder_common_tests` source list Task 1 touched.

- [ ] **Step 5: Build, run, commit**

```bash
cmake --build build/<preset> --target ladder_common_tests
QT_QPA_PLATFORM=offscreen ctest --test-dir build/<preset> -R busy --output-on-failure
git add examples/common/testkit/db_busy_fixture.hpp \
        examples/common/testkit/test_db_busy_fixture.cpp \
        examples/common/CMakeLists.txt
git commit -m "examples/common: add DbBusyFixture, resolving finding 018's SQLITE_BUSY gap"
```

---

## Task 8: `morph_add_rung()`'s real implementation, and `examples/pastebin/CMakeLists.txt`

**Files:**
- Modify: `cmake/morph_add_rung.cmake`
- Create: `examples/pastebin/CMakeLists.txt`

**Interfaces:**
- Produces: a working `morph_add_rung(NAME <rung>)` that convention-discovers
  and wires every target a rung might have — `ladder_<rung>_lib`,
  `ladder_<rung>_gui_lib`, `ladder_<rung>_gui`, `ladder_<rung>_gui_wasm`,
  `ladder_<rung>_server` (new: not in the rung-0 stub's original list — see
  below), `ladder_<rung>_tests`, `ladder_<rung>_headless` — building only
  the ones whose source directory actually has files, so this same function
  serves pastebin today and rung 2 onward unchanged. Tasks 9-13 add files
  under the directories this function globs; none of them touch CMake
  again.

**One generalization beyond the rung-0 stub's documented target list**: a
`ladder_<rung>_server` target (a standalone binary hosting the rung's
model(s) over a real `QtWebSocketServer`) — needed by every rung with a
WASM client, not just pastebin (the rung-0 WASM spike's own README already
anticipated this: "a standalone server binary hosting `SpikeEchoModel` for
the browser smoke would be built the same way"), so it belongs in the
shared function rather than being a pastebin-only bespoke addition.

- [ ] **Step 1: Rewrite `cmake/morph_add_rung.cmake`**

```cmake
# SPDX-License-Identifier: Apache-2.0
#
# morph_add_rung(NAME <rung>): scaffolds the standard target set for one
# ladder rung, per examples/TESTING.md "Build system and CI". Convention
# over configuration: every target below is created only if its source
# directory (relative to the caller's CMAKE_CURRENT_SOURCE_DIR, i.e.
# examples/<rung>/) actually has files — a rung with no gui_wasm/ yet simply
# gets no ladder_<rung>_gui_wasm target, silently, so this one function
# serves every rung from pastebin (rung 1) onward unchanged as each rung
# grows into more of the target set.
#
# Directory -> target convention:
#   src/models/*.cpp, src/db/*.cpp, src/app/*.cpp  -> ladder_<rung>_lib       STATIC (morph + Lightweight)
#   gui_lib/*.cpp                                  -> ladder_<rung>_gui_lib   STATIC (Qt6::Core only, no Catch2)
#   gui/*.cpp                                      -> ladder_<rung>_gui      EXE    (desktop client; skipped under Emscripten)
#   gui_wasm/*.cpp                                 -> ladder_<rung>_gui_wasm EXE    (Emscripten only)
#   src/server/*.cpp                                -> ladder_<rung>_server   EXE    (standalone server; skipped under Emscripten)
#   tests/*.cpp                                     -> ladder_<rung>_tests    EXE    (Catch2; skipped under Emscripten)
#   src/headless/*.cpp                              -> ladder_<rung>_headless EXE    (QProcess test-client binary, rung 4+)
#
# Every ctest case discovered from ladder_<rung>_tests gets labels "ladder"
# and "ladder-<rung>" (the CI path-filter unit — see .github/workflows/ci.yml,
# job ladder-tests) via the same two-step catch_discover_tests + file(GENERATE)
# shape examples/common/CMakeLists.txt uses (catch_discover_tests cannot carry
# a multi-value LABELS directly — see that file's own comment on why).
#
# RESOURCE_LOCK is the literal string "morph_ladder_test_db" for every rung's
# tests, matching examples/common's own ladder_common_tests — deliberately
# the *same* name across every rung/binary, not a per-rung one: ctest's
# RESOURCE_LOCK serializes any two ctest cases sharing a lock name even
# across different test *binaries*, which is exactly what's needed if two
# rungs' test binaries ever point at the same on-disk database file (e.g. a
# shared ODBC_CONNECTION_STRING override in some future CI leg) — harmless
# extra serialization if they don't.
function(morph_add_rung)
    set(options "")
    set(oneValueArgs NAME)
    set(multiValueArgs "")
    cmake_parse_arguments(RUNG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if(NOT RUNG_NAME)
        message(FATAL_ERROR "morph_add_rung() requires NAME <rung>")
    endif()
    if(NOT TARGET morph_ladder_testkit)
        message(FATAL_ERROR "morph_add_rung(NAME ${RUNG_NAME}) called before examples/common was added "
                            "(morph_ladder_testkit does not exist yet) — add_subdirectory(common) first.")
    endif()

    set(_dir "${CMAKE_CURRENT_SOURCE_DIR}")
    set(_rung "${RUNG_NAME}")

    # ── ladder_<rung>_lib: models + db + app bootstrap ──────────────────
    file(GLOB_RECURSE _lib_sources CONFIGURE_DEPENDS
        "${_dir}/src/models/*.cpp" "${_dir}/src/db/*.cpp" "${_dir}/src/app/*.cpp")
    if(_lib_sources)
        add_library(ladder_${_rung}_lib STATIC ${_lib_sources})
        add_library(morph::ladder_${_rung}_lib ALIAS ladder_${_rung}_lib)
        target_include_directories(ladder_${_rung}_lib PUBLIC "${_dir}/include")
        target_link_libraries(ladder_${_rung}_lib PUBLIC morph::morph Lightweight::Lightweight Qt6::Core)
        target_compile_features(ladder_${_rung}_lib PUBLIC cxx_std_23)
        set_target_properties(ladder_${_rung}_lib PROPERTIES AUTOMOC ON)
        # Lightweight's headers are not -Werror clean (bank's own caveat,
        # examples/bank/CMakeLists.txt) — no apply_warnings() here.
        if(AF_COVERAGE)
            apply_coverage(ladder_${_rung}_lib)
        endif()
    endif()

    # ── ladder_<rung>_gui_lib: presenters + forms-controller glue ───────
    file(GLOB_RECURSE _gui_lib_sources CONFIGURE_DEPENDS "${_dir}/gui_lib/*.cpp")
    if(_gui_lib_sources)
        add_library(ladder_${_rung}_gui_lib STATIC ${_gui_lib_sources})
        add_library(morph::ladder_${_rung}_gui_lib ALIAS ladder_${_rung}_gui_lib)
        target_include_directories(ladder_${_rung}_gui_lib PUBLIC "${_dir}/include" "${_dir}/gui_lib")
        target_link_libraries(ladder_${_rung}_gui_lib PUBLIC morph::morph morph::ladder_gui Qt6::Core)
        if(TARGET ladder_${_rung}_lib)
            target_link_libraries(ladder_${_rung}_gui_lib PUBLIC morph::ladder_${_rung}_lib)
        endif()
        target_compile_features(ladder_${_rung}_gui_lib PUBLIC cxx_std_23)
        set_target_properties(ladder_${_rung}_gui_lib PROPERTIES AUTOMOC ON)
        apply_warnings(ladder_${_rung}_gui_lib)
        if(AF_COVERAGE)
            apply_coverage(ladder_${_rung}_gui_lib)
        endif()
    endif()

    # ── ladder_<rung>_gui: desktop client (native only) ──────────────────
    if(NOT EMSCRIPTEN)
        file(GLOB_RECURSE _gui_sources CONFIGURE_DEPENDS "${_dir}/gui/*.cpp")
        if(_gui_sources AND TARGET ladder_${_rung}_gui_lib)
            find_package(Qt6 6.5 REQUIRED COMPONENTS Gui Qml Quick QuickControls2)
            qt_add_executable(ladder_${_rung}_gui ${_gui_sources})
            target_link_libraries(ladder_${_rung}_gui PRIVATE
                morph::ladder_${_rung}_gui_lib morph::ladder_app
                Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2)
            target_compile_features(ladder_${_rung}_gui PRIVATE cxx_std_23)
            set_target_properties(ladder_${_rung}_gui PROPERTIES AUTOMOC ON)
            if(AF_COVERAGE)
                apply_coverage(ladder_${_rung}_gui)
            endif()
        endif()
    endif()

    # ── ladder_<rung>_gui_wasm: Emscripten client ────────────────────────
    if(EMSCRIPTEN)
        file(GLOB_RECURSE _gui_wasm_sources CONFIGURE_DEPENDS "${_dir}/gui_wasm/*.cpp")
        if(_gui_wasm_sources)
            find_package(Qt6 REQUIRED COMPONENTS Gui Qml Quick QuickControls2)
            qt_add_executable(ladder_${_rung}_gui_wasm ${_gui_wasm_sources})
            target_link_libraries(ladder_${_rung}_gui_wasm PRIVATE
                morph::morph morph::qt morph_qt_impl morph::ladder_app
                Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2)
            if(TARGET ladder_${_rung}_gui_lib)
                target_link_libraries(ladder_${_rung}_gui_wasm PRIVATE morph::ladder_${_rung}_gui_lib)
            endif()
            target_compile_features(ladder_${_rung}_gui_wasm PRIVATE cxx_std_23)
        endif()
    endif()

    # ── ladder_<rung>_server: standalone server binary (native only) ────
    if(NOT EMSCRIPTEN)
        file(GLOB_RECURSE _server_sources CONFIGURE_DEPENDS "${_dir}/src/server/*.cpp")
        if(_server_sources AND TARGET ladder_${_rung}_lib)
            add_executable(ladder_${_rung}_server ${_server_sources})
            target_link_libraries(ladder_${_rung}_server PRIVATE
                morph::ladder_${_rung}_lib morph::qt morph_qt_impl Qt6::Core)
            target_compile_features(ladder_${_rung}_server PRIVATE cxx_std_23)
            if(AF_COVERAGE)
                apply_coverage(ladder_${_rung}_server)
            endif()
        endif()
    endif()

    # ── ladder_<rung>_tests: Catch2 model + presenter tests ──────────────
    if(NOT EMSCRIPTEN)
        file(GLOB_RECURSE _test_sources CONFIGURE_DEPENDS "${_dir}/tests/*.cpp")
        if(_test_sources)
            add_executable(ladder_${_rung}_tests ${_test_sources})
            target_link_libraries(ladder_${_rung}_tests PRIVATE morph::ladder_testkit)
            if(TARGET ladder_${_rung}_lib)
                target_link_libraries(ladder_${_rung}_tests PRIVATE morph::ladder_${_rung}_lib)
            endif()
            if(TARGET ladder_${_rung}_gui_lib)
                target_link_libraries(ladder_${_rung}_tests PRIVATE morph::ladder_${_rung}_gui_lib)
            endif()
            target_compile_features(ladder_${_rung}_tests PRIVATE cxx_std_23)
            set_target_properties(ladder_${_rung}_tests PROPERTIES AUTOMOC ON)
            apply_warnings(ladder_${_rung}_tests)
            if(AF_COVERAGE)
                apply_coverage(ladder_${_rung}_tests)
            endif()

            include(Catch)
            get_target_property(_qt_core_dll Qt6::Core IMPORTED_LOCATION)
            cmake_path(GET _qt_core_dll PARENT_PATH _qt_bin_dir)
            catch_discover_tests(ladder_${_rung}_tests
                DISCOVERY_MODE POST_BUILD
                DL_PATHS "${_qt_bin_dir}"
                PROPERTIES LABELS ladder TIMEOUT 120 RESOURCE_LOCK morph_ladder_test_db
            )
            file(GENERATE
                OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/ladder_${_rung}_tests_rung_label.cmake"
                CONTENT "foreach(_ladder_test IN LISTS ladder_${_rung}_tests_TESTS)
    if(NOT _ladder_test MATCHES \"\\\"class-name\\\"\")
        set_tests_properties(\"\${_ladder_test}\" PROPERTIES LABELS \"ladder;ladder-${_rung}\")
    endif()
endforeach()
"
            )
            set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES
                "${CMAKE_CURRENT_BINARY_DIR}/ladder_${_rung}_tests_rung_label.cmake")
        endif()
    endif()

    # ── ladder_<rung>_headless: QProcess test-client binary (rung 4+) ────
    file(GLOB_RECURSE _headless_sources CONFIGURE_DEPENDS "${_dir}/src/headless/*.cpp")
    if(_headless_sources AND TARGET ladder_${_rung}_gui_lib)
        add_executable(ladder_${_rung}_headless ${_headless_sources})
        target_link_libraries(ladder_${_rung}_headless PRIVATE morph::ladder_${_rung}_gui_lib morph::ladder_app)
        target_compile_features(ladder_${_rung}_headless PRIVATE cxx_std_23)
    endif()

    message(STATUS "morph_add_rung: registered rung '${_rung}'")
endfunction()
```

**Verify `qt_add_executable`'s availability/behavior** (it comes from
`qt_standard_project_setup`, already called in `examples/common/CMakeLists.txt`
for the whole ladder configure — confirm it doesn't need re-calling per
rung) and **`CONFIGURE_DEPENDS`'s support on every CI platform this repo
targets** (a Ninja/Makefiles-generator feature; the repo's presets use
Ninja per `apply_coverage`/`compiler_options.cmake` references seen this
session, so this should be safe, but confirm no preset uses a generator
where `CONFIGURE_DEPENDS` is silently ignored, which would mean a new
source file needs a manual reconfigure — document that caveat in this
file's header comment if so, rather than silently accepting stale builds).

- [ ] **Step 2: Write `examples/pastebin/CMakeLists.txt`**

```cmake
# SPDX-License-Identifier: Apache-2.0
#
# pastebin — rung 1 of the application ladder (examples/pastebin/README.md).
# All target wiring lives in morph_add_rung() (cmake/morph_add_rung.cmake);
# this file only pulls in pastebin-specific dependencies morph_add_rung()
# itself doesn't know about, then calls it.

cmake_minimum_required(VERSION 3.25)

morph_add_rung(NAME pastebin)
```

Everything else — Lightweight (already `FetchContent`-acquired once by
`examples/common/CMakeLists.txt`, per `TESTING.md`'s "hoisted once, not
repeated per rung"), Catch2, Qt6 WebSockets — is already available by the
time this file runs (`add_subdirectory(common)` in `examples/CMakeLists.txt`
runs before the rung loop). `Qt6::Gui`/`Qml`/`Quick`/`QuickControls2` are
pulled by `morph_add_rung()` itself, gated to only when `gui/`/`gui_wasm/`
actually have sources — pastebin's own `CMakeLists.txt` needs nothing
beyond the single `morph_add_rung(NAME pastebin)` call.

- [ ] **Step 3: Verify `examples/CMakeLists.txt` already lists `pastebin`**

It does (`_morph_known_rungs` already contains `pastebin`, from rung 0 —
confirm, no edit needed unless that list has drifted).

- [ ] **Step 4: Configure and build everything Tasks 1-7 already produced**

```bash
cmake --preset <preset> -DMORPH_BUILD_QT=ON -DMORPH_BUILD_LADDER=ON -DMORPH_LADDER_RUNGS=all
cmake --build build/<preset> --target ladder_pastebin_lib
```

Expect this to be the first point every earlier task's code actually
compiles as part of a real target — fix forward any remaining API
mismatches Task 5/6's "verify against real API" callouts flagged.

- [ ] **Step 5: Commit**

```bash
git add cmake/morph_add_rung.cmake examples/pastebin/CMakeLists.txt
git commit -m "cmake: implement morph_add_rung(), wire up examples/pastebin"
```

---

## Task 9: Model tests

**Files:**
- Create: `examples/pastebin/tests/test_paste_model.cpp`

**Interfaces:**
- Consumes: everything Tasks 1-8 produced. This is the first test binary in
  the repo to link `ladder_pastebin_lib` + `morph::ladder_testkit`.

Every required test from `examples/pastebin/README.md`'s "Required tests"
section, plus ordinary CRUD coverage for the model-coverage gate
(`IMPLEMENTATION.md` rule 5). Uses `morph::ladder::testkit::DbFixture`
(one per `TEST_CASE`, per rung 0's convention) and, where a test needs the
`Socket`-mode multi-client matrix, `morph::ladder::testkit::BackendRig`.

- [ ] **Step 1: Ordinary CRUD + validation, one `TEST_CASE` per action**

Straight-line: construct a `DbFixture`, build a `PasteModel` directly (no
`BridgeHandler` needed for these — call `model.execute(Action{...})`
in-process, synchronously, exactly like calling any plain method, since
`PasteModel::execute` is itself synchronous C++, not async) and assert the
result / thrown error. Cover: `CreatePaste` success and its `validate()`
rejection (empty content, empty syntax); `GetPaste` on a freshly created
paste (read count becomes 1, content matches); `GetPaste` against an
unknown id (`NotFound`); `EditPaste` on an editable paste (content
changes) and against a non-editable one (`ValidationError`) and an unknown
id (`NotFound`); `DeletePaste` then a follow-up `GetPaste` throws
`NotFound`; `ListPastes` returns only public pastes, respects the page
size, and `nextCursor` round-trips into a second call that returns the
remaining pastes with no overlap.

- [ ] **Step 2: Burn-after-read — the core semantics, single-client**

```cpp
TEST_CASE("GetPaste decrements the burn budget and deletes the paste on the last allowed read",
          "[pastebin][model]") {
    morph::ladder::testkit::DbFixture fixture;
    pastebin::PasteModel model;

    pastebin::CreatePaste create;
    create.content = "secret";
    create.syntax = "text";
    create.burnAfterReads = pastebin::Reads::fromDouble(2.0);
    const auto id = model.execute(create).id;

    const auto first = model.execute(pastebin::GetPaste{.id = id});
    CHECK(first.content == "secret");

    const auto second = model.execute(pastebin::GetPaste{.id = id});
    CHECK(second.content == "secret");  // still there — this was read 2 of 2, the burn happens after building the result

    REQUIRE_THROWS_AS(model.execute(pastebin::GetPaste{.id = id}), pastebin::NotFound);
}

TEST_CASE("GetPaste against an already-exhausted burn budget throws Burned, not NotFound, when the row still exists",
          "[pastebin][model]") {
    // Seeds a row directly at the storage layer with read_count already at
    // burn_after_reads, bypassing PasteModel::execute(GetPaste)'s own
    // delete-on-last-read step — this is exactly the "RETURNING zero rows"
    // classification branch Task 5/Task 7 both call out.
    morph::ladder::testkit::DbFixture fixture;
    {
        Lightweight::DataMapper mapper;
        pastebin::db::PasteRecord rec;
        rec.id = "test-burned-paste";
        rec.content = "gone";
        rec.syntax = "text";
        rec.createdAtMs = 0;
        rec.burnAfterReads = 1;
        rec.readCount = 1;  // already at budget
        mapper.Create(rec);
    }
    pastebin::PasteModel model;
    REQUIRE_THROWS_AS(model.execute(pastebin::GetPaste{.id = pastebin::PasteId{"test-burned-paste"}}), pastebin::Burned);
}
```

- [ ] **Step 3: Burn atomicity under concurrency — the race the README's
      design question exists for**

This is the test that fails the wrong way first if `PasteModel` used a
plain check-then-act instead of the atomic `UPDATE ... RETURNING`. Uses
`BackendRig{Socket, N}` (per-client, one `GetPaste` in flight each,
racing the same paste id) so the increment genuinely goes through separate
connections/sockets, not one in-process call stack:

```cpp
TEST_CASE("BackendRig::Socket: concurrent GetPaste calls against a burn-after-1 paste — exactly one client sees the content",
          "[pastebin][model][socket-only]") {
    morph::ladder::testkit::DbFixture fixture;
    pastebin::PasteModel seedModel;
    pastebin::CreatePaste create;
    create.content = "only one client should see this";
    create.syntax = "text";
    create.burnAfterReads = pastebin::Reads::fromDouble(1.0);
    const auto id = seedModel.execute(create).id;

    constexpr int kClients = 4;
    morph::ladder::testkit::BackendRig rig{morph::ladder::testkit::Mode::Socket, kClients};

    std::atomic<int> successes{0};
    std::atomic<int> notFounds{0};
    std::vector<morph::async::Completion<pastebin::PasteView>> pending;
    for (int i = 0; i < kClients; ++i) {
        auto handler = rig.client<pastebin::PasteModel>(i);
        pending.push_back(std::move(handler.execute(pastebin::GetPaste{.id = id})));
    }
    for (auto& completion : pending) {
        std::move(completion)
            .then([&](pastebin::PasteView) { successes.fetch_add(1); })
            .onError([&](const std::exception_ptr&) { notFounds.fetch_add(1); });
    }

    REQUIRE(morph::ladder::testkit::pumpUntil([&] { return successes.load() + notFounds.load() == kClients; }));
    CHECK(successes.load() == 1);
    CHECK(notFounds.load() == kClients - 1);
}
```

**Verify `BackendRig::client<Model>(index)` returns something whose
`.execute(...)` can be moved into a `std::vector` of pending completions
the way sketched** (check `examples/common/testkit/test_backend_rig.cpp`'s
own usage for the real pattern — every existing usage awaits one call at a
time; racing N concurrent calls against one `BackendRig` may need a
different composition than the sketch above, e.g. keeping each client's
`BridgeHandler` alive in its own named variable rather than a vector of
completions — adjust to what actually compiles and genuinely races, and
keep the race-provoking property: all N `GetPaste` calls issued before any
of them is awaited).

- [ ] **Step 4: Expiry — via the injectable clock, no real sleeping**

```cpp
TEST_CASE("A paste past its expiresAt throws Expired from GetPaste, even before the sweep runs",
          "[pastebin][model]") {
    morph::ladder::testkit::DbFixture fixture;
    pastebin::PasteModel model;

    pastebin::CreatePaste create;
    create.content = "expiring";
    create.syntax = "text";
    create.expiresAt = morph::ladder::now();  // "now" at creation time
    const auto id = model.execute(create).id;

    // Advance the injected clock past expiresAt — no sweep involved yet,
    // proving GetPaste's own atomic WHERE clause is what enforces this,
    // matching the README's "correctness doesn't depend on sweep timing".
    morph::ladder::ScopedClockOverride later{*(*morph::ladder::now().value + std::chrono::hours{1})};
    REQUIRE_THROWS_AS(model.execute(pastebin::GetPaste{.id = id}), pastebin::Expired);
}

TEST_CASE("App's periodic sweep dispatches ExpirePaste for a past-expiry paste, and it is gone afterward",
          "[pastebin][app]") {
    morph::ladder::testkit::DbFixture fixture;
    pastebin::PasteModel model;
    pastebin::CreatePaste create;
    create.content = "to be swept";
    create.syntax = "text";
    create.expiresAt = morph::ladder::now();
    const auto id = model.execute(create).id;

    morph::ladder::ScopedClockOverride later{*(*morph::ladder::now().value + std::chrono::hours{1})};

    pastebin::app::App app{std::filesystem::temp_directory_path() / "pastebin_sweep_test.jsonl",
                            std::chrono::hours{1} /* disable the timer; call sweepExpiredOnce() directly */};
    app.sweepExpiredOnce();
    REQUIRE(morph::ladder::testkit::pumpUntil([&] {
        try {
            model.execute(pastebin::GetPaste{.id = id});
            return false;  // still there
        } catch (const pastebin::NotFound&) {
            return true;  // swept
        }
    }));
}
```

**Verify `App`'s constructor and `sweepExpiredOnce()` compose correctly
with a `DbFixture`-backed database** — `App` constructs its own
`RemoteServer`/worker pool against whatever the *default* connection
currently is (set by `DbFixture`'s construction earlier in this test),
which should just work since both go through the same
`Lightweight::SqlConnection::SetDefaultConnectionString` global — confirm
no ordering surprise when writing this test for real.

- [ ] **Step 5: Duplicate create on retry (weaker approximation, per README)**

```cpp
TEST_CASE("A resent CreatePaste with the same content does not mint two pastes under this rung's weaker double-execute guard",
          "[pastebin][model]") {
    // README: "Until the fault-injection proxy exists (rung 4), this is
    // explicitly the weaker approximation — double-execute with the same
    // op id — not true reply-frame loss." Rung 1 does not yet have an
    // idempotency-key field on CreatePaste (that lands at rung 4 per
    // LADDER.md's "exactly-once delivery" strain). This test documents
    // today's honest behavior instead of asserting a guarantee the rung
    // does not implement: two independent CreatePaste calls with identical
    // content ARE two distinct pastes today (no dedup key exists yet) —
    // assert that fact plainly, so this test fails loudly the day rung 4's
    // idempotency-key discipline lands here and this comment/test need
    // updating together, rather than silently drifting.
    morph::ladder::testkit::DbFixture fixture;
    pastebin::PasteModel model;
    pastebin::CreatePaste create;
    create.content = "resent";
    create.syntax = "text";
    const auto first = model.execute(create).id;
    const auto second = model.execute(create).id;
    CHECK(*first.value != *second.value);
}
```

**This deliberately documents a known limitation rather than the stronger
guarantee the README's "Required tests" bullet originally gestured at** —
re-read that bullet against `PasteModel`'s actual DTOs (Task 3 has no
op-id/idempotency-key field on `CreatePaste`, correctly, since the README
scopes that discipline to rung 4) before writing this test for real, and
resolve the tension in favor of testing what the shipped code actually
does, not a guarantee it was never asked to provide.

- [ ] **Step 6: Id-collision handling in the tiny animal-name keyspace**

```cpp
TEST_CASE("CreatePaste retries past a colliding animal-name id instead of failing the whole call",
          "[pastebin][model]") {
    morph::ladder::testkit::DbFixture fixture;
    // Pre-seed a row occupying one specific id from the keyspace so the
    // very next CreatePaste has a real chance of colliding on its first
    // attempt — the retry loop (Task 5) must recover from that, not
    // propagate the constraint-violation exception. Given the keyspace's
    // small, enumerable size (Task 5's kAdjectives x kAnimals x 1000
    // suffixes), a single pre-seeded id makes a first-attempt collision
    // plausible but not guaranteed within one run; the assertion below
    // only requires CreatePaste to succeed at all (proving the retry loop
    // works when a collision *does* happen), not that a collision
    // necessarily happened this run — REQUIRE_NOTHROW across many
    // repeated calls is the practical way to exercise the retry path
    // without depending on a specific RNG draw.
    Lightweight::DataMapper mapper;
    pastebin::db::PasteRecord seed;
    seed.id = "bold-cat-1";  // must match a real, reachable combination from Task 5's tables
    seed.content = "occupying this id";
    seed.syntax = "text";
    seed.createdAtMs = 0;
    seed.readCount = 0;
    mapper.Create(seed);

    pastebin::PasteModel model;
    for (int i = 0; i < 50; ++i) {
        pastebin::CreatePaste create;
        create.content = "attempt " + std::to_string(i);
        create.syntax = "text";
        REQUIRE_NOTHROW(model.execute(create));
    }
}
```

- [ ] **Step 7: Size-limit UX**

Construct a `BackendRig{Socket}` (the message-size bound is enforced at
`QtWebSocketServer`, not the model — see `include/morph/qt/qt_websocket_server.hpp`'s
`maxMessageBytes`), issue a `CreatePaste` whose `content` exceeds a small,
test-configured `maxMessageBytes`, and assert the client's `Completion`
rejects with a message containing `"message exceeds maxMessageBytes"`
(the exact server-side string, confirmed this session). **Verify
`BackendRig` exposes a way to configure `QtWebSocketServerConfig::maxMessageBytes`
for its internal `Socket`-mode server** — if it does not, this is a small,
legitimate `examples/common/testkit/backend_rig.hpp` extension (an
optional config parameter alongside the existing `authorizer` one), not a
pastebin-only workaround; make that addition here if needed, with its own
test in `test_backend_rig.cpp`.

- [ ] **Step 8: Hostile content round-trip**

```cpp
TEST_CASE("Hostile fuzz-corpus content round-trips through CreatePaste/GetPaste unchanged, both backends",
          "[pastebin][model]") {
    auto mode = GENERATE(morph::ladder::testkit::Mode::Local, morph::ladder::testkit::Mode::Socket);
    morph::ladder::testkit::DbFixture fixture;
    morph::ladder::testkit::BackendRig rig{mode, 1};
    auto handler = rig.client<pastebin::PasteModel>(0);

    for (const auto& corpusFile : {"tests/fuzz/findings/dispatch_execute/err_reply_control_char_roundtrip.bin",
                                    "tests/fuzz/findings/wire_decode/skip_ws_heap_overflow.bin"}) {
        std::ifstream in{corpusFile, std::ios::binary};
        REQUIRE(in.good());
        const std::string content{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};

        pastebin::CreatePaste create;
        create.content = content;
        create.syntax = "text";
        const auto id = morph::ladder::testkit::awaitQt(handler.execute(create)).id;
        const auto fetched = morph::ladder::testkit::awaitQt(handler.execute(pastebin::GetPaste{.id = id}));
        CHECK(fetched.content == content);
    }
}
```

**Verify the corpus file paths resolve from `ladder_pastebin_tests`'
working directory** (ctest's default working directory is the build tree's
per-target directory, not the repo root — the existing corpus-consuming
fuzz harness, if any, or `tests/`'s own CMake wiring likely already solves
"find the repo root from a test binary"; check `tests/CMakeLists.txt` for
the convention already in use — e.g. a compiled-in
`CMAKE_SOURCE_DIR`-derived constant — rather than a fragile relative path
guess).

- [ ] **Step 9: Security posture — fail-open delta**

```cpp
TEST_CASE("Fail-open default: an unauthenticated client can register and execute against a learned paste id",
          "[pastebin][security]") {
    // Executable documentation of docs/spec/security.md's fail-open
    // default (rung 1 deliberately does not configure an authorizer) —
    // this asserts the *documented* behavior, not a bug: any client can
    // read a paste it knows the id of, with no session at all.
    morph::ladder::testkit::DbFixture fixture;
    pastebin::PasteModel seedModel;
    pastebin::CreatePaste create;
    create.content = "no auth configured";
    create.syntax = "text";
    const auto id = seedModel.execute(create).id;

    morph::ladder::testkit::BackendRig rig{morph::ladder::testkit::Mode::Socket, 1};  // no authorizer arg -> AllowAllAuthorizer
    auto handler = rig.client<pastebin::PasteModel>(0);
    const auto fetched = morph::ladder::testkit::awaitQt(handler.execute(pastebin::GetPaste{.id = id}));
    CHECK(fetched.content == "no auth configured");
}
```

- [ ] **Step 10: `hello` protocol-version negotiation**

```cpp
TEST_CASE("hello negotiates the server's configured protocol version range",
          "[pastebin][security]") {
    morph::ladder::testkit::DbFixture fixture;
    morph::ladder::testkit::BackendRig rig{morph::ladder::testkit::Mode::Socket, 1};
    // The rig's own backend for client 0 is already connected; negotiate
    // over it directly (QtWebSocketBackend::negotiateProtocolVersion(),
    // confirmed this session — native-test-only, blocks via a nested
    // QEventLoop, exactly what a native Catch2 test wants).
    // Verify BackendRig exposes the raw QtWebSocketBackend* (or add a
    // narrow accessor if it currently only exposes the Bridge/handler) —
    // needed to call negotiateProtocolVersion() directly.
}
```

**This step is intentionally left as a directed spec, not full code** — it
needs `BackendRig`'s exact `Socket`-mode internals (whether the raw
`QtWebSocketBackend*` is reachable) confirmed against
`examples/common/testkit/backend_rig.hpp` while writing it; add a narrow
accessor there (with its own `test_backend_rig.cpp` case) if none exists,
the same way Step 7 above may need one for `maxMessageBytes`.

- [ ] **Step 11: Store-error branch coverage — using Task 7's `DbBusyFixture`**

```cpp
TEST_CASE("GetPaste's atomic update surfaces a real SQLITE_BUSY as a thrown error, not silent data loss",
          "[pastebin][model]") {
    morph::ladder::testkit::DbFixture fixture;
    pastebin::PasteModel model;
    pastebin::CreatePaste create;
    create.content = "contended";
    create.syntax = "text";
    const auto id = model.execute(create).id;

    morph::ladder::testkit::DbBusyFixture busy{"pastes"};
    REQUIRE_THROWS(model.execute(pastebin::GetPaste{.id = id}));
}
```

Plus the `UNIQUE`-violation and zero-rows-affected classification cases
already covered by Steps 2 and 6 above (per Task 7's own note: those two
need no new fixture).

- [ ] **Step 12: Add the new test file to `examples/pastebin`'s test target**

`morph_add_rung()` (Task 8) already globs `tests/*.cpp` — no CMake edit
needed, just placing the file under `examples/pastebin/tests/`.

- [ ] **Step 13: Build, run, measure coverage**

```bash
cmake --build build/<preset> --target ladder_pastebin_tests
QT_QPA_PLATFORM=offscreen ctest --test-dir build/<preset> -L ladder-pastebin --output-on-failure
```

Then extend `scripts/coverage.sh`'s `SOURCES` array (already
conditionally includes `examples/common`) to also include
`examples/pastebin/include`/`examples/pastebin/src` when
`ladder_pastebin_tests` exists, following the exact same
`if [ -x "$LADDER_TEST_EXE" ]` guard pattern the script already uses —
and extend `codecov.yml`'s `ladder` component's `paths` list the same way
(or add a second component, `pastebin`, if the team prefers per-rung gates
— either is consistent with `IMPLEMENTATION.md` rule 5; pick one and note
the choice in the commit message). Per rule 5's own guidance from the
rung-0 coverage work: measure the real ceiling via `llvm-cov export`'s
JSON, document every known-artifact line, and set the target from that
measurement — do not assume a blind 100% target will pass.

- [ ] **Step 14: Commit**

```bash
git add examples/pastebin/tests/test_paste_model.cpp \
        scripts/coverage.sh codecov.yml
git commit -m "pastebin: add PasteModel tests (CRUD, burn atomicity, expiry, security, coverage)"
```

---

## Task 10: Presenters and the forms-controller glue

**Files:**
- Create: `examples/pastebin/gui_lib/paste_presenter.hpp`
- Create: `examples/pastebin/gui_lib/paste_presenter.cpp`
- Create: `examples/pastebin/gui_lib/paste_forms_controller.hpp`
- Create: `examples/pastebin/gui_lib/paste_forms_controller.cpp`

**Interfaces:**
- Consumes: `examples/common/gui::Presenter` (`track()`/`busy()`/`idle()`),
  `pastebin::PasteModel`/DTOs, `morph::forms::schemaJson<A>()`.
- Produces: `pastebin::gui::PastePresenter` (routes create/get/edit/delete/
  list through a `BridgeHandler<PasteModel>`, surfaces typed errors) and
  `pastebin::gui::PasteFormsController` (the finding-021 workaround: same
  `schemaJson()`/`submitIfValid()`/`fetchOptions()` surface as the shipped
  `FormsControllerCore`, but composed over an injected `Bridge&`/
  `IExecutor*` instead of constructing its own backend). Task 11 (presenter
  tests) and Task 12 (GUI shell) both consume these.

`TESTING.md`'s presenter rule 2 binds both classes: neither constructs a
`Bridge`, an executor, or a backend — both take `(Bridge&, IExecutor*)` (or
a pre-built `BridgeHandler`) from whatever composes them, which is always
`examples/common/gui::AppContext::onReady(...)` at the GUI-shell layer
(Task 12).

- [ ] **Step 1: Write `examples/pastebin/gui_lib/paste_presenter.hpp`**

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "common/gui/presenter.hpp"
#include "pastebin/dto/paste_dto.hpp"
#include "pastebin/models/paste_model.hpp"

#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>

namespace pastebin::gui {

/// @brief Routes CreatePaste/GetPaste/EditPaste/DeletePaste/ListPastes
///        through a `BridgeHandler<PasteModel>`, surfacing typed errors to
///        whatever view composes this (QML properties/signals, Task 12).
///        Translates and routes only — no domain logic
///        (`IMPLEMENTATION.md` rule 2).
class PastePresenter : public ::morph::ladder::gui::Presenter {
    Q_OBJECT
  public:
    /// @param bridge   The shared `Bridge` `AppContext` owns.
    /// @param executor The executor `Completion` callbacks land on.
    /// @param parent   Optional `QObject` parent.
    PastePresenter(::morph::bridge::Bridge& bridge, ::morph::exec::IExecutor* executor, QObject* parent = nullptr);

    void create(CreatePaste action);
    void get(GetPaste action);
    void edit(EditPaste action);
    void remove(DeletePaste action);
    void list(ListPastes action);

  signals:
    void created(CreatePasteResult result);
    void loaded(PasteView view);
    void edited(PasteView view);
    void removed();
    void listed(ListPastesResult result);
    /// @brief Emitted for any action's typed error — @p message is
    ///        `std::exception::what()`, ready for direct display.
    void failed(QString message);

  private:
    ::morph::bridge::BridgeHandler<PasteModel> _handler;
};

}  // namespace pastebin::gui
```

- [ ] **Step 2: Write `examples/pastebin/gui_lib/paste_presenter.cpp`**

Each method follows `Presenter::track()`'s documented composition order
(its own doc comment, Task-1-adjacent research this session: `track()`'s
internal `.onError` only decrements the busy counter — a subclass wanting
to *display* the error must attach its own `.onError` **before** handing
the completion to `track()`, since `track()` is the last handler attached
and takes the completion by value):

```cpp
// SPDX-License-Identifier: Apache-2.0
#include "paste_presenter.hpp"

namespace pastebin::gui {

PastePresenter::PastePresenter(::morph::bridge::Bridge& bridge, ::morph::exec::IExecutor* executor, QObject* parent)
    : Presenter{parent}, _handler{bridge, executor} {}

void PastePresenter::create(CreatePaste action) {
    track<CreatePasteResult>(
        _handler.execute(std::move(action)).onError([this](const std::exception_ptr& err) {
            try {
                std::rethrow_exception(err);
            } catch (const std::exception& e) {
                emit failed(QString::fromStdString(e.what()));
            }
        }),
        [this](CreatePasteResult result) { emit created(std::move(result)); });
}

void PastePresenter::get(GetPaste action) {
    track<PasteView>(
        _handler.execute(std::move(action)).onError([this](const std::exception_ptr& err) {
            try {
                std::rethrow_exception(err);
            } catch (const std::exception& e) {
                emit failed(QString::fromStdString(e.what()));
            }
        }),
        [this](PasteView view) { emit loaded(std::move(view)); });
}

void PastePresenter::edit(EditPaste action) {
    track<PasteView>(
        _handler.execute(std::move(action)).onError([this](const std::exception_ptr& err) {
            try {
                std::rethrow_exception(err);
            } catch (const std::exception& e) {
                emit failed(QString::fromStdString(e.what()));
            }
        }),
        [this](PasteView view) { emit edited(std::move(view)); });
}

void PastePresenter::remove(DeletePaste action) {
    track<Ack>(
        _handler.execute(std::move(action)).onError([this](const std::exception_ptr& err) {
            try {
                std::rethrow_exception(err);
            } catch (const std::exception& e) {
                emit failed(QString::fromStdString(e.what()));
            }
        }),
        [this](Ack) { emit removed(); });
}

void PastePresenter::list(ListPastes action) {
    track<ListPastesResult>(
        _handler.execute(std::move(action)).onError([this](const std::exception_ptr& err) {
            try {
                std::rethrow_exception(err);
            } catch (const std::exception& e) {
                emit failed(QString::fromStdString(e.what()));
            }
        }),
        [this](ListPastesResult result) { emit listed(std::move(result)); });
}

}  // namespace pastebin::gui
```

**This duplicates the same six-line try/catch-and-emit block five times —
after it compiles and passes its Task-11 tests, consider (in this same
task, not deferred) factoring it into one private helper
(`template <typename T> auto reportErrors()` returning the `onError`
lambda, or a member function taking the completion) if doing so doesn't
fight `track<T>`'s own template-argument deduction** — note in the task
report which shape was kept.

- [ ] **Step 3: Write `examples/pastebin/gui_lib/paste_forms_controller.hpp`**

The finding-021 workaround — same public surface as
`morph::qt::forms::FormsControllerCore<Model>`
(`include/morph/qt/forms/forms_controller_core.hpp`), composed over an
injected `Bridge&`/`IExecutor*` instead of a hardcoded `LocalBackend`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "pastebin/models/paste_model.hpp"

#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>

#include <string>

namespace pastebin::gui {

/// @brief Same schema-driven surface as the shipped
///        `morph::qt::forms::FormsControllerCore<PasteModel>`
///        (`schemaJson()`/`submitIfValid()`/`fetchOptions()`), composed
///        over an injected `Bridge&`/`IExecutor*` instead of constructing
///        its own `LocalBackend` — the shipped core cannot do this
///        (finding 021), and `TESTING.md`'s presenter rule 2 forbids GUI
///        code from constructing its own backend/executor, so this rung
///        owns a thin, otherwise-identical controller instead. Pure glue,
///        no domain logic (`IMPLEMENTATION.md` rule 2 justification (b)) —
///        the schema/validation/rendering machinery is untouched; only the
///        backend-wiring seam differs.
class PasteFormsController {
  public:
    /// @param bridge   The shared `Bridge` `AppContext` owns.
    /// @param executor The executor `Completion` callbacks land on.
    /// @param schemasJson Pre-assembled `{actionType: schemaJson<A>()}` map,
    ///        matching `FormsControllerCore`'s own constructor contract.
    PasteFormsController(::morph::bridge::Bridge& bridge, ::morph::exec::IExecutor* executor, std::string schemasJson);

    [[nodiscard]] const std::string& schemasJson() const noexcept { return _schemasJson; }

    template <typename OnReply, typename OnError>
    void submitIfValid(std::string actionType, std::string bodyJson, OnReply onReply, OnError onError);

  private:
    ::morph::bridge::BridgeHandler<PasteModel> _handler;
    std::string _schemasJson;
};

}  // namespace pastebin::gui
```

**Verify `FormsControllerCore`'s real `submitIfValid`/`fetchOptions`
template signatures and bodies against
`include/morph/qt/forms/forms_controller_core.hpp` before writing this
file's real implementation** — only the class *shape* (member list,
constructor pattern) was confirmed this session, not the two template
methods' full bodies (they were described, not quoted verbatim). Copy
their real logic (schema lookup by `actionType`, JSON body validation
against that schema, dispatch through `_handler`) verbatim, changing only
how `_handler` gets its `Bridge`/executor. If `fetchOptions` turns out to
be needed by any pastebin form (check whether any DTO field uses
`morph::forms::Choice<T,...>` — Task 3's DTOs do not, per this plan's own
design, so `fetchOptions` may not be needed at all for rung 1; omit it if
so, and say so in the task report rather than stubbing an unused method).

- [ ] **Step 4: Write `examples/pastebin/gui_lib/paste_forms_controller.cpp`**

Implements `submitIfValid` (and `fetchOptions` only if Step 3 determined
it's needed) against the real `FormsControllerCore` logic adapted per
Step 3's note.

- [ ] **Step 5: Build**

```bash
cmake --build build/<preset> --target ladder_pastebin_gui_lib
```

- [ ] **Step 6: Commit**

```bash
git add examples/pastebin/gui_lib/
git commit -m "pastebin: add PastePresenter and the finding-021 forms-controller glue"
```

---

## Task 11: Presenter tests

**Files:**
- Create: `examples/pastebin/tests/test_paste_presenter.cpp`

**Interfaces:**
- Consumes: Task 10's `PastePresenter`, rung 0's `BackendRig`/`pumpUntil`/
  `settle`-equivalent pattern (`Presenter::busy()`).

Full backend-mode matrix (`Local`/`LocalSingleThread`/`Socket`, via
`GENERATE`, per `TESTING.md`), one `TEST_CASE` per presenter method plus
the `failed` signal path:

- [ ] **Step 1: Write the matrix test**

```cpp
// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "paste_presenter.hpp"
#include "testkit/backend_rig.hpp"
#include "testkit/db_fixture.hpp"
#include "testkit/pump.hpp"

TEST_CASE("PastePresenter::create then get round-trips a paste, all three backend modes",
          "[pastebin][presenter]") {
    auto mode = GENERATE(morph::ladder::testkit::Mode::Local, morph::ladder::testkit::Mode::LocalSingleThread,
                          morph::ladder::testkit::Mode::Socket);
    morph::ladder::testkit::DbFixture fixture;
    morph::ladder::testkit::BackendRig rig{mode, 1};
    auto bridge = rig.bridge(0);
    pastebin::gui::PastePresenter presenter{*bridge, rig.executor()};

    pastebin::PasteId createdId;
    bool created = false;
    QObject::connect(&presenter, &pastebin::gui::PastePresenter::created,
                      [&](pastebin::CreatePasteResult result) {
                          createdId = result.id;
                          created = true;
                      });
    pastebin::CreatePaste create;
    create.content = "presenter round-trip";
    create.syntax = "text";
    presenter.create(create);
    REQUIRE(morph::ladder::testkit::pumpUntil([&] { return created; }));
    REQUIRE_FALSE(presenter.busy());

    pastebin::PasteView loaded;
    bool gotLoaded = false;
    QObject::connect(&presenter, &pastebin::gui::PastePresenter::loaded, [&](pastebin::PasteView view) {
        loaded = view;
        gotLoaded = true;
    });
    presenter.get(pastebin::GetPaste{.id = createdId});
    REQUIRE(morph::ladder::testkit::pumpUntil([&] { return gotLoaded; }));
    CHECK(loaded.content == "presenter round-trip");
}

TEST_CASE("PastePresenter::get against an unknown id emits failed, not a crash", "[pastebin][presenter]") {
    morph::ladder::testkit::DbFixture fixture;
    morph::ladder::testkit::BackendRig rig{morph::ladder::testkit::Mode::Local, 1};
    auto bridge = rig.bridge(0);
    pastebin::gui::PastePresenter presenter{*bridge, rig.executor()};

    QString failure;
    bool failed = false;
    QObject::connect(&presenter, &pastebin::gui::PastePresenter::failed, [&](QString message) {
        failure = message;
        failed = true;
    });
    presenter.get(pastebin::GetPaste{.id = pastebin::PasteId{"no-such-paste"}});
    REQUIRE(morph::ladder::testkit::pumpUntil([&] { return failed; }));
    CHECK_FALSE(failure.isEmpty());
    REQUIRE_FALSE(presenter.busy());
}
```

**Verify `BackendRig::bridge(index)`'s exact return type** (a `Bridge*` or
`Bridge&` — `PastePresenter`'s constructor above takes `Bridge&`, adjust
the dereference accordingly) **against `examples/common/testkit/backend_rig.hpp`**
before writing this for real; extend with `edit`/`remove`/`list` cases
following the same shape.

- [ ] **Step 2: One offscreen QML engine-load smoke test**

Per `TESTING.md` presenter rule 6 ("one offscreen engine-load smoke test
(engine creates root object, no errors) registered in ctest — not Qt Quick
Test") — this depends on Task 12's QML file existing, so **defer writing
this specific test's body until Task 12 lands**; create the file now with
a one-line comment marking it deferred, or fold this step into Task 12
instead if that reads more naturally once Task 12's QML file path is
known. Either placement is fine; do not skip the test itself.

- [ ] **Step 3: Build, run, commit**

```bash
cmake --build build/<preset> --target ladder_pastebin_tests
QT_QPA_PLATFORM=offscreen ctest --test-dir build/<preset> -L ladder-pastebin --output-on-failure
git add examples/pastebin/tests/test_paste_presenter.cpp
git commit -m "pastebin: add PastePresenter tests (full backend-mode matrix)"
```

---

## Task 12: Desktop GUI shell, standalone server binary, demo seeding

**Files:**
- Create: `examples/pastebin/gui/main.cpp`
- Create: `examples/pastebin/gui/qml/Main.qml`
- Create: `examples/pastebin/gui/qml/PasteView.qml`
- Create: `examples/pastebin/src/server/main.cpp`
- Create/modify: `examples/pastebin/tests/test_gui_qml_smoke.cpp` (Task 11
  Step 2's deferred test, if not already written there)

**Interfaces:**
- Consumes: Task 10's `PastePresenter`/`PasteFormsController`,
  `examples/common/gui::AppContext`, the real `MorphForms` QML module,
  Task 6's `App`.
- Produces: a running desktop client and a standalone server process —
  the first point in this rung where the whole loop is manually
  end-to-end verifiable, not just unit-tested.

Follow `examples/forms/gui_qml/`'s real, working shape (`Main.qml`'s
`import MorphForms`, `FormsController { id: formsController }`,
`JSON.parse(formsController.schemasJson)` — confirmed this session) for
the QML side, substituting `pastebin::gui::PasteFormsController` for that
demo's `FormsController` type (Task 10 gave it the same public surface on
purpose) and `pastebin::gui::PastePresenter` for whatever list/detail view
state the schema-driven form doesn't cover (paste content display,
burn/expiry status — `IMPLEMENTATION.md` rule 2's "pure glue" allowance;
these are read-only displays of server-computed state, not hand-rolled
input widgets).

- [ ] **Step 1: Write `examples/pastebin/gui/main.cpp`**

Wires `AppContext` (`Mode = Remote{url}` from a `--server` CLI arg,
defaulting to `Local{workers=4}` — mirroring `AppContext`'s own doc-comment
example construction pattern from rung 0), constructs `PastePresenter`/
`PasteFormsController` inside `ctx.onReady([&] { ... })`, exposes them to
QML via `QQmlApplicationEngine::rootContext()->setContextProperty(...)`,
loads `qrc:/pastebin/qml/Main.qml` (or the QML-module URI form
`examples/forms/gui_qml/CMakeLists.txt`'s `qt_add_qml_module` call uses —
match that exact convention, including whatever URI naming scheme it
established, e.g. `Pastebin` as this rung's own module name).

- [ ] **Step 2: Write the QML files**

`Main.qml`: app shell + the schema-driven create form (`DynamicForm` from
`MorphForms`, per that module's real QML API — read
`src/qt/forms/qml/DynamicForm.qml`'s documented usage before wiring this).
`PasteView.qml`: read-only display of a fetched `PasteView` (content,
syntax, burn/expiry status) — plain `Text`/`ScrollView`, zero styling
effort (`IMPLEMENTATION.md` rule 2: "Default Qt Quick controls, default
fonts, no theming").

- [ ] **Step 3: Write the offscreen QML smoke test** (Task 11 Step 2)

```cpp
// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <QGuiApplication>
#include <QQmlApplicationEngine>

TEST_CASE("pastebin's QML engine loads Main.qml and creates a root object with no errors",
          "[pastebin][gui][qml-smoke]") {
    QQmlApplicationEngine engine;
    bool hadError = false;
    QObject::connect(&engine, &QQmlApplicationEngine::warnings, [&](const QList<QQmlError>&) { hadError = true; });
    engine.load(QUrl{"qrc:/pastebin/qml/Main.qml"});  // match Step 1's real module/resource URI
    REQUIRE_FALSE(engine.rootObjects().isEmpty());
    REQUIRE_FALSE(hadError);
}
```

Runs under `QT_QPA_PLATFORM=offscreen` (already set for the whole
`ladder-tests`/`clang-coverage` CI legs — no per-test setup needed).

- [ ] **Step 4: Write `examples/pastebin/src/server/main.cpp`**

```cpp
// SPDX-License-Identifier: Apache-2.0
#include "pastebin/app/app.hpp"
#include "pastebin/db/database.hpp"

#include <morph/qt/qt_websocket_server.hpp>

#include <QCoreApplication>

#include <cstdlib>
#include <filesystem>
#include <iostream>

int main(int argc, char** argv) {
    QCoreApplication qtApp{argc, argv};

    const char* connectionString = std::getenv("PASTEBIN_DB");
    pastebin::db::setup(connectionString != nullptr ? connectionString
                                                      : "DRIVER=SQLite3;Database=pastebin.db;Timeout=5000");

    pastebin::app::App app{std::filesystem::current_path() / "pastebin_actions.jsonl"};

    const char* portEnv = std::getenv("PASTEBIN_PORT");
    const int port = portEnv != nullptr ? std::atoi(portEnv) : 0;
    morph::qt::QtWebSocketServer wsServer{*app.server(), port};
    if (!wsServer.listen()) {
        std::cerr << "pastebin-server: failed to listen\n";
        return 1;
    }
    std::cout << "pastebin-server: listening on port " << wsServer.port() << '\n';

    return QCoreApplication::exec();
}
```

**Verify `morph::qt::QtWebSocketServer`'s real constructor and `listen()`/
`port()` API** against `include/morph/qt/qt_websocket_server.hpp` — this
sketch follows the shape `examples/common/testkit/backend_rig.hpp`'s own
`Socket`-mode construction already uses successfully in this codebase
(`QtWebSocketServer{*server, 0}` then `.listen()`/`.port()`), so it should
transcribe directly; confirm the exact argument order.

- [ ] **Step 5: Demo seeding**

Per `LADDER.md`'s "every rung ships a `--seed` path" operations
convention: add a `--seed` flag to the server binary (Step 4) that, after
`pastebin::db::setup()`, calls `PasteModel::execute(CreatePaste{...})`
directly (in-process, synchronous — no need for a `Bridge`/handler) a
handful of times with representative content (a few public pastes, one
with `burnAfterReads` set, one with `expiresAt` set) before starting the
WebSocket listener. `action_driver.hpp`'s generator machinery is
explicitly **rung 4**'s deliverable (`TESTING.md`'s component table) — do
not pull it forward for this; a half-dozen hardcoded `CreatePaste` calls
is the right-sized answer here, matching the README's "keep the rung-1
answer primitive" framing used elsewhere in this plan.

- [ ] **Step 6: Manual end-to-end verification**

```bash
cmake --build build/<preset> --target ladder_pastebin_server ladder_pastebin_gui
./build/<preset>/examples/pastebin/ladder_pastebin_server --seed &
./build/<preset>/examples/pastebin/ladder_pastebin_gui --server ws://127.0.0.1:<port>
```

Confirm: the desktop client's create form submits and lists the seeded +
newly created pastes; opening one increments its read count; a
burn-after-1 seeded paste disappears after one open. Record the outcome
(including any real failure — this is genuinely unverified machinery, like
the `RETURNING` and `SQLITE_BUSY` spikes earlier) in the task report.

- [ ] **Step 7: Commit**

```bash
git add examples/pastebin/gui/ examples/pastebin/src/server/ examples/pastebin/tests/test_gui_qml_smoke.cpp
git commit -m "pastebin: add desktop GUI shell, standalone server binary, demo seeding"
```

---

## Task 13: WASM client, CI wiring, and the final docs pass

**Files:**
- Create: `examples/pastebin/gui_wasm/main_wasm.cpp`
- Modify: `.github/workflows/ci.yml` (confirm/extend the `ladder-tests` job's
  WASM compile-gate matrix to include pastebin, if not already generic)
- Modify: `examples/pastebin/README.md` (final DoD checklist, status)
- Modify: `examples/TESTING.md` (only if this task's real experience
  contradicts anything it currently states — read it fresh against what
  actually shipped before editing)

**Interfaces:**
- Produces: rung 1's WASM client — **same client code as the desktop
  shell** (`PastePresenter`/`PasteFormsController`/the QML files Task 12
  wrote), only `main_wasm.cpp` differs (per rung 0's own hard requirement:
  copying bank's `gui_wasm` shadow-header pattern is forbidden —
  `TESTING.md`'s "Do not copy bank's `gui_wasm` shadow-header pattern").

This is rung 1's payoff on rung 0's WASM-remote spike
(`examples/common/wasm_spike/`): the spike proved
`QtWebSocketBackend`+`asyncRegistrationEnabled` works from WASM in
isolation (unverified against a real Emscripten toolchain per its own
README) — Task 6's `App` and rung 0's `AppContext` already wrap that exact
pattern generically, so pastebin's WASM client should need **no
WASM-specific application code at all**, only a WASM-specific `main()`.

- [ ] **Step 1: Write `examples/pastebin/gui_wasm/main_wasm.cpp`**

```cpp
// SPDX-License-Identifier: Apache-2.0
#include "common/gui/app_context.hpp"
#include "paste_forms_controller.hpp"
#include "paste_presenter.hpp"

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QUrl>

#include <optional>

int main(int argc, char** argv) {
    QGuiApplication qtApp{argc, argv};

    // The WASM client is always Remote — there is no in-process server to
    // be Local against in a browser (IMPLEMENTATION.md rule 4's WASM
    // clause: persistence lives server-side, behind the model).
    morph::ladder::gui::AppContext ctx{
        morph::ladder::gui::AppContext::Remote{QUrl{MORPH_LADDER_PASTEBIN_WASM_SERVER_URL}}};

    QQmlApplicationEngine engine;
    std::optional<pastebin::gui::PastePresenter> presenter;
    std::optional<pastebin::gui::PasteFormsController> formsController;
    ctx.onReady([&] {
        presenter.emplace(ctx.bridge(), ctx.executor());
        formsController.emplace(ctx.bridge(), ctx.executor(), /* same schemasJson assembly as Task 12's main.cpp */ std::string{});
        engine.rootContext()->setContextProperty("pastePresenter", &*presenter);
        engine.rootContext()->setContextProperty("pasteFormsController", &*formsController);
        engine.load(QUrl{"qrc:/pastebin/qml/Main.qml"});
    });

    return QGuiApplication::exec();
}
```

**`MORPH_LADDER_PASTEBIN_WASM_SERVER_URL`** is a compile-definition, set by
this task's CMake addition — follow
`examples/common/wasm_spike/CMakeLists.txt`'s own
`MORPH_LADDER_WASM_SPIKE_SERVER_URL` convention exactly (same mechanism,
new name) rather than inventing a different configuration path.
**Duplicate the exact `schemasJson` assembly Task 12's `gui/main.cpp` uses**
for `formsController`'s construction — both binaries must build the
identical schema map, so factor it into one shared free function
(`examples/pastebin/gui_lib/paste_schemas.hpp`, a small addition to this
task alongside `main_wasm.cpp`) that both `main.cpp` and `main_wasm.cpp`
call, rather than duplicating the assembly logic inline in each.

- [ ] **Step 2: Confirm `morph_add_rung()` already builds this under Emscripten**

Task 8's `morph_add_rung()` globs `gui_wasm/*.cpp` under its
`if(EMSCRIPTEN)` branch already — no CMake edit needed beyond what Step 1
places on disk, **unless** the WASM build needs the compile definition
from Step 1's note, in which case add exactly that one
`target_compile_definitions(ladder_pastebin_gui_wasm PRIVATE
MORPH_LADDER_PASTEBIN_WASM_SERVER_URL="${MORPH_LADDER_PASTEBIN_WASM_SERVER_URL}")`
line to `examples/pastebin/CMakeLists.txt` (following
`wasm_spike/CMakeLists.txt`'s exact pattern), guarded the same way that
file guards it (only meaningful under `EMSCRIPTEN`).

- [ ] **Step 3: Attempt a real Emscripten configure/build**

```bash
emcmake cmake --preset <wasm-preset-or-manual-configure> -DMORPH_BUILD_LADDER=ON -DMORPH_LADDER_RUNGS=pastebin
cmake --build build/<wasm-preset> --target ladder_pastebin_gui_wasm
```

Per rung 0's own WASM spike precedent: **if no Emscripten toolchain is
available in this environment, or the build fails**, do not silently work
around it — this is the same class of "real, unverified machinery" the
spike itself flagged. Follow the spike's own documented fallback protocol
(`examples/common/wasm_spike/README.md`'s "Fallback plan" section,
already read in full this session): identify which failure mode it is
(configure failure / page-abort / hang-with-no-result — adapted to a build
failure if the toolchain issue surfaces at compile time instead of
runtime), file it as a finding with the concrete error captured, and mark
this task's step complete with a "documents a real blocker" note rather
than blocking the whole rung's exit on an environment limitation outside
this codebase's control. If it **does** build and run (via `emrun` +
manual browser check, mirroring the spike's own manual-verification
steps), that closes out rung 0's WASM-remote proof for real application
code, not just the spike's echo model — note this explicitly, since it is
the first time this has happened in this codebase.

- [ ] **Step 4: Confirm CI's `ladder-tests` job picks pastebin up**

Read `.github/workflows/ci.yml`'s `ladder-tests` job (added in rung 0) —
per `TESTING.md`'s "Build system and CI" section, it should already be
generic (`MORPH_LADDER_RUNGS` path-filtered, no per-rung job edits
needed). If it genuinely is generic, this step is a read-only
confirmation, no diff. If it turns out rung 0 left something rung-specific
stubbed (e.g. a hardcoded rung list, or the WASM compile gate only ever
exercising the spike, not real rung `gui_wasm` targets), fix that gap here
— this is finding-018/021-shaped territory (a real gap in
already-shipped infrastructure) if it exists, not a pastebin-only patch.

- [ ] **Step 5: Final docs pass**

Update `examples/pastebin/README.md`: flip `**Status: in progress.**` to
`**Status: rung 1 shipped.**` (or whatever this repo's convention for a
finished rung turns out to be — check whether any other rung README uses
a "done" status marker as precedent; if none does, this is the first, so
pick a plain, honest phrase), and tick off every "Definition of done" bullet
against what actually shipped — including being honest about anything that
did **not** fully land (an unverified `RETURNING`/`SQLITE_BUSY`/Emscripten
spike result is not a failure of this task, but it must be stated plainly,
matching this whole plan's "verify, don't assume" thread throughout).

- [ ] **Step 6: Commit**

```bash
git add examples/pastebin/gui_wasm/ examples/pastebin/gui_lib/paste_schemas.hpp \
        examples/pastebin/CMakeLists.txt examples/pastebin/README.md \
        .github/workflows/ci.yml
git commit -m "pastebin: add WASM client, confirm CI wiring, close out rung 1's DoD"
```

---

## Post-plan: findings review

Before the final whole-branch review (per `subagent-driven-development`'s
process), re-read every finding this plan may have touched —
`003`/`018`/`020`/`021` at minimum — and update each one's `disposition`
field to match what actually shipped (e.g. `018` moves from `open` to
`documented-limitation` or stays `open` depending on whether `DbBusyFixture`
actually worked; `020`/`021` almost certainly stay `open` — they are real
framework gaps this rung worked around, not framework changes this rung
made). Per `FINDINGS.md`'s triage rule, disposition decisions are the repo
owner's call, not something this plan pre-decides — flag each one's
recommended disposition in the final review's report rather than editing
the frontmatter unilaterally for any finding whose disposition isn't
already obvious from this plan's own text.

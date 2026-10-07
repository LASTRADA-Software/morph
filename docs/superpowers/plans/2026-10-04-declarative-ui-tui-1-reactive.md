# Declarative UI, Part 1 — `morph::reactive` Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for
> tracking.

**Goal:** Ship `morph::reactive` — a glitch-free signal graph, `Store<ViewState, Msg>`, and declarative control
(`Query`, `Mutation`, `Subscription`) — header-only in the base `morph` target, with headless Catch2 tests.

**Architecture:** A push-dirty/pull-value graph (Clean/Check/Dirty colouring) whose nodes share one
`detail::RuntimeCore`. Every write ends a batch; the outermost batch posts **one** flush to the owner executor;
the flush pulls queued effects through their computeds. Misuse is reported through
`morph::exec::detail::noteOwner` and then refused. `Query`/`Mutation`/`Subscription` are built from signals, an
effect and a `CallbackScope`, over any `Completion`-returning fetcher (a `BridgeHandler` in production, a
settleable promise in tests).

**Tech Stack:** C++23, header-only; morph core (`IExecutor`, `OwnerAffinity`, `noteOwner`, `Completion`,
`CallbackScope`, `BridgeHandler`); Catch2 v3.

**Spec:** `docs/superpowers/specs/2026-10-04-declarative-ui-tui-design.md` (§3, §4, §4b, §7, §9).

This is **Part 1 of 11** of the declarative-UI program. Read the master plan
`docs/superpowers/plans/2026-10-04-declarative-ui-tui.md` first: it fixes the branch, the commit layout and the
`wip(...)` commit convention every task below follows, and this part ends by squashing its `wip(reactive)` commits
into one.

## Global Constraints

- C++23. `morph::reactive` is header-only, part of the base `morph` target's `FILE_SET HEADERS`; no new CMake
  option, no new dependency.
- Every file starts with `// SPDX-License-Identifier: Apache-2.0`, then `#pragma once` for headers.
- Naming (`.clang-tidy`): types `CamelCase`, functions and variables `camelBack`, private members `_camelBack`,
  public data members `camelBack`, constants `kName`; identifiers at least 3 characters except `i j k x y n fn cb op`.
- Comments and docs state what the code does now and why — no history, no issue numbers, no commit hashes.
- Doxygen runs with `WARN_AS_ERROR = FAIL_ON_WARNINGS` over every `*.hpp`, `detail` included: every class,
  function and data member gets a brief, and every function complete `@param`/`@tparam`/`@return`.
- `-Weverything -Werror` (Clang) and clang-tidy (`WarningsAsErrors: "*"`) must stay clean.
- The UI state type is `ViewState`, never `Model` (morph's `Model` is the bridge domain model).
- Misuse site strings are exactly those in `detail::site` (Task 1); tests count them through
  `morph::testing::OwnerProbeRecorder`.
- Commits end with `Signed-off-by: Christian Parpart <christian@parpart.family>`.

## Review Focus

1. **An Effect that writes a signal it reads** must not loop; it runs once per external change (Task 1 test
   "an Effect that writes what it reads does not re-run itself").
2. **A node destroyed while queued in the running flush** (a Switch remount destroying a child effect) must not
   be touched again (Task 1 test "a queued Effect destroyed during the flush is skipped").
3. **A batch whose body throws** must still flush the writes made before the throw, once, and leave the batch
   depth at zero (Task 1 test "a throwing batch body still flushes what it wrote").
4. **A Query key function that throws** is an Effect throw: reported, and the query keeps its last state (Task 6
   test "a throwing key is reported and leaves the query as it was").
5. **A Mutation invalidating an idle Query** (key `nullopt`) issues nothing (Task 7 test "invalidating an idle
   query issues nothing").

---

## File Structure

| File | Responsibility |
|---|---|
| `include/morph/reactive/detail/graph.hpp` | `Colour`, `site::` names, `RuntimeCore` (affinity, batch depth, queue, flush), `TrackingFrame`, `Node`, `BatchScope`, `WidgetEventScope` |
| `include/morph/reactive/runtime.hpp` | `RuntimeOptions`, `Runtime` (`batch`, `widgetEvent`, `untracked`) |
| `include/morph/reactive/signal.hpp` | `Signal<T>`, `Computed<T>`, `Effect` |
| `include/morph/reactive/scope.hpp` | `Scope` |
| `include/morph/reactive/store.hpp` | `ExhaustiveUpdate`, `Store<ViewState, Msg>`, `request()` |
| `include/morph/reactive/control.hpp` | `errorMessage`, `Refetchable`, `Query<A, R>`, `MutationOptions`, `Mutation<A, R>`, `Subscription<R>` |
| `tests/test_reactive_signal.cpp` | Signal, Effect, batching, scheduling |
| `tests/test_reactive_computed.cpp` | Computed, glitch-freedom, throwing computeds |
| `tests/test_reactive_misuse.cpp` | Every reported-then-refused misuse |
| `tests/test_reactive_scope.cpp` | Scope ownership and teardown order |
| `tests/test_reactive_store.cpp` | Store, `ExhaustiveUpdate`, `request()` over a `LocalBackend` |
| `tests/test_reactive_query.cpp` | Query over settleable completions |
| `tests/test_reactive_mutation.cpp` | Mutation, Subscription-free cases, `errorMessage` |
| `tests/test_reactive_control_bridge.cpp` | Query, Mutation, Subscription, `refreshOn` over a real `LocalBackend` |
| `docs/spec/reactive/signals.md`, `store.md`, `control.md` | Authoritative specs |
| `CMakeLists.txt`, `tests/CMakeLists.txt` | Header and test registration |
| `docs/spec/README.md`, `docs/ARCHITECTURE.md`, `CHANGELOG.md` | Maps and changelog |

## Build and test commands (used by every task)

```bash
cmake -S . -B build/reactive -G Ninja -DCMAKE_BUILD_TYPE=Debug -DMORPH_BUILD_EXAMPLES=OFF   # once
cmake --build build/reactive --target morph_tests
./build/reactive/tests/morph_tests "[reactive]"
```

Configuring prints `morph: warnings: ... strict=ON`; if it does not, stop and say so (CONTRIBUTING, "Warnings
are errors").

---
### Task 1: The graph core, `Runtime`, `Signal` and `Effect`

**Files:**
- Create: `include/morph/reactive/detail/graph.hpp`
- Create: `include/morph/reactive/runtime.hpp`
- Create: `include/morph/reactive/signal.hpp`
- Modify: `CMakeLists.txt` — the `FILE_SET HEADERS` block of `target_sources(morph …)` (the one listing
  `include/morph/version.hpp` first) and the `FILE_SET morph_detail_headers` block
- Modify: `tests/CMakeLists.txt` — the `add_executable(morph_tests …)` source list
- Test: `tests/test_reactive_signal.cpp`

**Interfaces:**
- Consumes: `morph::exec::IExecutor` (`include/morph/core/executor.hpp`),
  `morph::exec::detail::OwnerAffinity` (`core/detail/owner_affinity.hpp`),
  `morph::exec::detail::noteOwner` (`core/detail/owner_probe.hpp`), `MORPH_LIFETIMEBOUND` (`attributes.hpp`).
- Produces (later tasks rely on these exact names):
  - `morph::reactive::detail::site::{kOffOwner, kEffectThrew, kWriteCycle, kSetInComputed,
    kComputedReadsItself, kSendInUpdate, kRuntimeOutlived, kFlushInWidgetEvent}` — `char const*` constants.
  - `detail::Colour { Clean, Check, Dirty }`.
  - `detail::RuntimeCore`: `checkOwner() -> bool`, `report(char const*)`, `owner()`, `isComputing()`,
    `enterComputed()`, `leaveComputed()`, `tracking()`, `isFlushRequested()`, `liveNodes()`.
  - `detail::TrackingFrame(RuntimeCore&, Node* observer)`, `takeSources()`.
  - `detail::Node(std::shared_ptr<RuntimeCore>)`: public `markStale(Colour)`, `updateIfNecessary()`,
    `countRun(std::uint64_t)`, `settle()`; protected `core()`, `trackRead()`, `forceDirty()`,
    `markObserversStale(Colour)`, `adoptSources(std::vector<Node*>)`, `mergeSources(std::vector<Node*>)`,
    pure virtual `recompute() -> bool`, virtual `isEffect() -> bool`.
  - `detail::BatchScope(RuntimeCore&)`, `detail::WidgetEventScope(RuntimeCore&)`.
  - `reactive::RuntimeOptions { std::size_t maxEffectRunsPerFlush = 100; std::function<void()> afterFlush; }`.
  - `reactive::Runtime(exec::IExecutor&, RuntimeOptions = {})`: `batch(F)`, `widgetEvent(F)`, `untracked(F)`,
    `owner()`, `isFlushRequested()`, `core() -> std::shared_ptr<detail::RuntimeCore> const&`.
  - `reactive::Signal<T>(Runtime&)`, `Signal<T>(Runtime&, T)`: `get()`, `peek()`, `set(T)`, `mutate(F)`.
  - `reactive::Effect(Runtime&, F)`.

- [ ] **Step 1: Write the failing test**

Create `tests/test_reactive_signal.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/signal.hpp>
#include <stdexcept>
#include <type_traits>
#include <vector>

#include "test_support.hpp"

namespace {

using morph::reactive::Effect;
using morph::reactive::Runtime;
using morph::reactive::RuntimeOptions;
using morph::reactive::Signal;
using Owner = morph::testing::StepExecutor;

// No operator==: the equality skip must not apply to it.
struct Opaque {
    int value = 0;
};

}  // namespace

static_assert(!std::is_copy_constructible_v<Signal<int>> && !std::is_move_constructible_v<Signal<int>>);
static_assert(!std::is_copy_assignable_v<Signal<int>> && !std::is_move_assignable_v<Signal<int>>);
static_assert(!std::is_copy_constructible_v<Effect> && !std::is_move_constructible_v<Effect>);
static_assert(!std::is_copy_constructible_v<Runtime> && !std::is_move_constructible_v<Runtime>);

TEST_CASE("reactive::Signal: get returns the initial value and set replaces it", "[reactive]") {
    Owner owner;
    Runtime runtime{owner};
    Signal<int> count{runtime, 1};
    CHECK(count.get() == 1);
    count.set(2);
    CHECK(count.peek() == 2);
}

TEST_CASE("reactive::Effect: runs on construction, then in a posted flush after a source changes", "[reactive]") {
    Owner owner;
    Runtime runtime{owner};
    Signal<int> count{runtime, 1};
    std::vector<int> seen;
    Effect const log{runtime, [&] { seen.push_back(count.get()); }};
    CHECK(seen == std::vector{1});

    count.set(2);
    CHECK(seen == std::vector{1});
    owner.runAll();
    CHECK(seen == std::vector{1, 2});
}

TEST_CASE("reactive::Runtime: nested batches are one flush and one run", "[reactive]") {
    Owner owner;
    Runtime runtime{owner};
    Signal<int> first{runtime, 0};
    Signal<int> second{runtime, 0};
    int runs = 0;
    Effect const sum{runtime, [&] {
                         static_cast<void>(first.get() + second.get());
                         ++runs;
                     }};
    runtime.batch([&] {
        first.set(1);
        second.set(2);
        runtime.batch([&] { first.set(3); });
        CHECK(owner.pending() == 0);
    });
    CHECK(owner.pending() == 1);
    owner.runAll();
    CHECK(runs == 2);
}

TEST_CASE("reactive::Runtime: exactly one post per idle-to-pending transition", "[reactive]") {
    Owner owner;
    Runtime runtime{owner};
    Signal<int> value{runtime, 0};
    Effect const watch{runtime, [&] { static_cast<void>(value.get()); }};
    value.set(1);
    value.set(2);
    value.set(3);
    CHECK(owner.pending() == 1);
    CHECK(runtime.isFlushRequested());
    owner.runAll();
    CHECK_FALSE(runtime.isFlushRequested());
    value.set(4);
    CHECK(owner.pending() == 1);
}

TEST_CASE("reactive::Signal: an equal value notifies nobody; a non-comparable one always does", "[reactive]") {
    Owner owner;
    Runtime runtime{owner};
    Signal<int> comparable{runtime, 1};
    Signal<Opaque> opaque{runtime, Opaque{1}};
    Effect const watch{runtime, [&] {
                           static_cast<void>(comparable.get());
                           static_cast<void>(opaque.get());
                       }};
    comparable.set(1);
    CHECK(owner.pending() == 0);
    opaque.set(Opaque{1});
    CHECK(owner.pending() == 1);
}

TEST_CASE("reactive::Signal: mutate always notifies", "[reactive]") {
    Owner owner;
    Runtime runtime{owner};
    Signal<std::vector<int>> list{runtime, {}};
    std::size_t lastSize = 99;
    Effect const watch{runtime, [&] { lastSize = list.get().size(); }};
    CHECK(lastSize == 0);
    list.mutate([](std::vector<int>& items) { items.push_back(7); });
    owner.runAll();
    CHECK(lastSize == 1);
}

TEST_CASE("reactive::Effect: a source it stops reading no longer triggers it", "[reactive]") {
    Owner owner;
    Runtime runtime{owner};
    Signal<bool> useFirst{runtime, true};
    Signal<int> first{runtime, 0};
    Signal<int> second{runtime, 0};
    int runs = 0;
    Effect const pick{runtime, [&] {
                          ++runs;
                          static_cast<void>(useFirst.get() ? first.get() : second.get());
                      }};
    useFirst.set(false);
    owner.runAll();
    CHECK(runs == 2);

    first.set(1);
    CHECK(owner.pending() == 0);

    second.set(1);
    owner.runAll();
    CHECK(runs == 3);
}

TEST_CASE("reactive::Effect: destruction unlinks it from its sources", "[reactive]") {
    Owner owner;
    Runtime runtime{owner};
    Signal<int> value{runtime, 0};
    { Effect const transient{runtime, [&] { static_cast<void>(value.get()); }}; }
    value.set(1);
    CHECK(owner.pending() == 0);
}

TEST_CASE("reactive::Signal: destruction unlinks it from its observers", "[reactive]") {
    Owner owner;
    Runtime runtime{owner};
    auto source = std::make_unique<Signal<int>>(runtime, 0);
    Signal<int> other{runtime, 0};
    int runs = 0;
    Effect const watch{runtime, [&] {
                           ++runs;
                           static_cast<void>(other.get());
                           if (source) {
                               static_cast<void>(source->get());
                           }
                       }};
    source.reset();
    other.set(1);
    owner.runAll();  // the effect re-tracks without the destroyed source; ASan would flag a dangling link
    CHECK(runs == 2);
}

TEST_CASE("reactive::Runtime: a flush posted before the Runtime is destroyed is a no-op", "[reactive]") {
    Owner owner;
    int runs = 0;
    {
        Runtime runtime{owner};
        Signal<int> value{runtime, 0};
        Effect const watch{runtime, [&] {
                               static_cast<void>(value.get());
                               ++runs;
                           }};
        value.set(1);
        CHECK(owner.pending() == 1);
    }
    owner.runAll();
    CHECK(runs == 1);
}

TEST_CASE("reactive::Runtime: afterFlush runs once per flush that ran effects", "[reactive]") {
    Owner owner;
    int frames = 0;
    Runtime runtime{owner, RuntimeOptions{.afterFlush = [&] { ++frames; }}};
    Signal<int> value{runtime, 0};
    Effect const watch{runtime, [&] { static_cast<void>(value.get()); }};
    value.set(1);
    value.set(2);
    owner.runAll();
    CHECK(frames == 1);
}

TEST_CASE("reactive::Runtime: untracked reads do not subscribe", "[reactive]") {
    Owner owner;
    Runtime runtime{owner};
    Signal<int> value{runtime, 0};
    int runs = 0;
    Effect const watch{runtime, [&] {
                           ++runs;
                           static_cast<void>(runtime.untracked([&] { return value.get(); }));
                       }};
    value.set(1);
    CHECK(owner.pending() == 0);
    CHECK(runs == 1);
}

// Review Focus 1.
TEST_CASE("reactive::Effect: an Effect that writes what it reads does not re-run itself", "[reactive]") {
    Owner owner;
    Runtime runtime{owner};
    Signal<int> trigger{runtime, 0};
    Signal<int> counter{runtime, 0};
    int runs = 0;
    Effect const bump{runtime, [&] {
                          static_cast<void>(trigger.get());
                          counter.set(counter.get() + 1);
                          ++runs;
                      }};
    trigger.set(1);
    owner.runAll();
    CHECK(runs == 2);
    CHECK(counter.peek() == 2);
    CHECK(owner.pending() == 0);
}

// Review Focus 2.
TEST_CASE("reactive::Effect: a queued Effect destroyed during the flush is skipped", "[reactive]") {
    Owner owner;
    Runtime runtime{owner};
    Signal<int> value{runtime, 0};
    std::unique_ptr<Effect> victim;
    int victimRuns = 0;
    Effect const killer{runtime, [&] {
                            if (value.get() == 1) {
                                victim.reset();
                            }
                        }};
    victim = std::make_unique<Effect>(runtime, [&] {
        static_cast<void>(value.get());
        ++victimRuns;
    });
    value.set(1);  // queues killer, then victim
    owner.runAll();
    CHECK(victimRuns == 1);
    CHECK(victim == nullptr);
}

// Review Focus 3.
TEST_CASE("reactive::Runtime: a throwing batch body still flushes what it wrote", "[reactive]") {
    Owner owner;
    Runtime runtime{owner};
    Signal<int> value{runtime, 0};
    std::vector<int> seen;
    Effect const watch{runtime, [&] { seen.push_back(value.get()); }};
    CHECK_THROWS_AS(runtime.batch([&] {
        value.set(5);
        throw std::runtime_error{"body failed"};
    }),
                    std::runtime_error);
    CHECK(owner.pending() == 1);
    owner.runAll();
    CHECK(seen == std::vector{0, 5});
    value.set(6);  // the batch depth returned to zero, so this posts again
    CHECK(owner.pending() == 1);
}
```

Register it: in `tests/CMakeLists.txt`, add `test_reactive_signal.cpp` to the `add_executable(morph_tests …)` list
directly after `test_callback_scope.cpp`.

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build/reactive --target morph_tests`
Expected: FAIL — `fatal error: 'morph/reactive/runtime.hpp' file not found`.

- [ ] **Step 3: Write the graph core**

Create `include/morph/reactive/detail/graph.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include "../../attributes.hpp"
#include "../../core/detail/owner_affinity.hpp"
#include "../../core/detail/owner_probe.hpp"
#include "../../core/executor.hpp"

/// @file
/// @brief The reactive graph's internals: node colours and links, the tracking
///        frame, batching and the flush queue.
///
/// Specified in `docs/spec/reactive/signals.md`.

namespace morph::reactive::detail {

/// @brief The site names a misuse is reported under through `exec::detail::noteOwner`.
namespace site {
/// @brief An operation ran off the Runtime's owner; it is dropped.
inline constexpr char const* kOffOwner = "morph::reactive: off the Runtime's owner";
/// @brief An exception escaped an Effect; the flush stops.
inline constexpr char const* kEffectThrew = "morph::reactive: an exception escaped an Effect";
/// @brief An Effect re-ran more than `maxEffectRunsPerFlush` times in one flush; the flush stops.
inline constexpr char const* kWriteCycle = "morph::reactive: an Effect re-ran past maxEffectRunsPerFlush";
/// @brief A signal was written while a Computed was computing; the write is dropped.
inline constexpr char const* kSetInComputed = "morph::reactive: set() inside a Computed";
/// @brief A Computed read itself; the read throws `std::logic_error`.
inline constexpr char const* kComputedReadsItself = "morph::reactive: a Computed read itself";
/// @brief `Store::send` was called from inside an update; the message is dropped.
inline constexpr char const* kSendInUpdate = "morph::reactive: send() inside an update";
/// @brief A Runtime was destroyed while nodes made from it were alive.
inline constexpr char const* kRuntimeOutlived = "morph::reactive: Runtime destroyed while nodes are alive";
/// @brief A flush ran inside a widget event; it is re-posted.
inline constexpr char const* kFlushInWidgetEvent = "morph::reactive: flush inside a widget event";
}  // namespace site

/// @brief A node's freshness. Ordered: a later enumerator is staler.
enum class Colour : std::uint8_t {
    Clean,  ///< Up to date.
    Check,  ///< A transitive source changed: pull the sources before deciding to recompute.
    Dirty,  ///< A direct source changed: recompute.
};

class Node;
class TrackingFrame;

/// @brief State shared by one `Runtime` and every node made from it.
///
/// Nodes hold it by `shared_ptr`, so a node that outlives its `Runtime` (a
/// reported misuse) still has a valid core. The posted flush holds it weakly
/// and checks `detach()`, so a flush after the `Runtime` is gone does nothing.
class RuntimeCore : public std::enable_shared_from_this<RuntimeCore> {
public:
    /// @param owner The executor every operation must run on, and the one flushes are posted to.
    ///        Borrowed: it must outlive the core.
    explicit RuntimeCore(exec::IExecutor& owner MORPH_LIFETIMEBOUND) noexcept : _affinity{owner} {}

    ~RuntimeCore() = default;
    RuntimeCore(RuntimeCore const&) = delete;
    RuntimeCore& operator=(RuntimeCore const&) = delete;
    RuntimeCore(RuntimeCore&&) = delete;
    RuntimeCore& operator=(RuntimeCore&&) = delete;

    /// @brief Whether the calling thread is on the owner, reporting `site::kOffOwner` when it is not.
    /// @return True on the owner: inside one of its tasks, or on the thread that built the Runtime
    ///         outside every executor's task (a Qt slot, `main()`, a test body).
    [[nodiscard]] bool checkOwner() const noexcept {
        if (_affinity.here()) {
            return true;
        }
        report(site::kOffOwner);
        return false;
    }

    /// @brief Reports a misuse: asserts in a debug build, or hands it to an installed owner probe.
    /// @param where One of the `site::` constants.
    void report(char const* where) const noexcept {
        exec::detail::noteOwner(where, _affinity.owner().coreExecutor(), false);
    }

    /// @brief The owner executor.
    /// @return The executor passed at construction.
    [[nodiscard]] exec::IExecutor& owner() const noexcept { return _affinity.owner(); }

    /// @brief Sets the flush bound and the after-flush hook.
    /// @param maxEffectRunsPerFlush How often one Effect may run in a flush before it is a write cycle.
    /// @param afterFlush Called after every flush that ran at least one Effect; may be empty.
    void configure(std::size_t maxEffectRunsPerFlush, std::function<void()> afterFlush) {
        _maxEffectRunsPerFlush = maxEffectRunsPerFlush;
        _afterFlush = std::move(afterFlush);
    }

    /// @brief Opens a batch; batches nest.
    void beginBatch() noexcept { ++_batchDepth; }

    /// @brief Closes a batch; closing the outermost one requests a flush when effects are queued.
    void endBatch() {
        if (--_batchDepth == 0) {
            requestFlush();
        }
    }

    /// @brief Queues an Effect that turned stale.
    /// @param effect The Effect node.
    void enqueue(Node& effect) { _queue.push_back(&effect); }

    /// @brief Posts one flush to the owner, unless one is already requested or running, nothing is
    ///        queued, or the Runtime is gone.
    void requestFlush() {
        if (_flushing || _isFlushRequested || _queue.empty() || _detached) {
            return;
        }
        _affinity.owner().post([weak = weak_from_this()] {
            if (auto const core = weak.lock()) {
                core->flush();
            }
        });
        // Set only once the post succeeded: a post that fails to allocate leaves the queue to the
        // next write.
        _isFlushRequested = true;
    }

    /// @brief Runs every queued Effect that a source change really affects. Defined below `Node`.
    void flush();

    /// @brief Removes every reference the core holds to a node being destroyed. Defined below `Node`.
    /// @param node The node.
    void forget(Node& node) noexcept;

    /// @brief The innermost tracking frame, or null outside every tracked run.
    /// @return The frame reads register with.
    [[nodiscard]] TrackingFrame* tracking() const noexcept { return _tracking; }

    /// @brief Installs the innermost tracking frame.
    /// @param frame The frame, or null.
    void setTracking(TrackingFrame* frame) noexcept { _tracking = frame; }

    /// @brief Whether a Computed is computing on this runtime.
    /// @return True between `enterComputed()` and the matching `leaveComputed()`.
    [[nodiscard]] bool isComputing() const noexcept { return _computing != 0; }

    /// @brief Marks a Computed as computing.
    void enterComputed() noexcept { ++_computing; }

    /// @brief Marks a Computed as done computing.
    void leaveComputed() noexcept { --_computing; }

    /// @brief Marks the start of a widget callback.
    void enterWidgetEvent() noexcept { ++_widgetEventDepth; }

    /// @brief Marks the end of a widget callback.
    void leaveWidgetEvent() noexcept { --_widgetEventDepth; }

    /// @brief Whether a flush is posted and has not run yet.
    /// @return The coalescing flag.
    [[nodiscard]] bool isFlushRequested() const noexcept { return _isFlushRequested; }

    /// @brief Marks the Runtime as gone: flushes do nothing from now on.
    void detach() noexcept { _detached = true; }

    /// @brief Counts a node made from this runtime.
    void nodeCreated() noexcept { ++_liveNodes; }

    /// @brief Counts a node destroyed.
    void nodeDestroyed() noexcept { --_liveNodes; }

    /// @brief How many nodes made from this runtime are alive.
    /// @return The live-node count.
    [[nodiscard]] std::size_t liveNodes() const noexcept { return _liveNodes; }

private:
    void dropQueue() noexcept;

    exec::detail::OwnerAffinity _affinity;
    std::deque<Node*> _queue;
    std::function<void()> _afterFlush;
    TrackingFrame* _tracking = nullptr;
    std::size_t _maxEffectRunsPerFlush = 100;
    std::size_t _batchDepth = 0;
    std::size_t _computing = 0;
    std::size_t _widgetEventDepth = 0;
    std::size_t _liveNodes = 0;
    std::uint64_t _flushEpoch = 0;
    bool _isFlushRequested = false;
    bool _flushing = false;
    bool _detached = false;
};

/// @brief Collects the sources one tracked run reads, and restores the previous frame when it ends.
///
/// A frame whose observer is null is an untracked region: reads inside it subscribe nothing.
class TrackingFrame {
public:
    /// @param core The runtime core.
    /// @param observer The node whose run this is, or null for an untracked region.
    TrackingFrame(RuntimeCore& core, Node* observer) noexcept
        : _core{&core}, _previous{core.tracking()}, _observer{observer} {
        core.setTracking(this);
    }

    ~TrackingFrame() { _core->setTracking(_previous); }
    TrackingFrame(TrackingFrame const&) = delete;
    TrackingFrame& operator=(TrackingFrame const&) = delete;
    TrackingFrame(TrackingFrame&&) = delete;
    TrackingFrame& operator=(TrackingFrame&&) = delete;

    /// @brief Records that the run read @p source; a no-op in an untracked region.
    /// @param source The node read.
    void add(Node& source) {
        if (_observer != nullptr && std::ranges::find(_sources, &source) == _sources.end()) {
            _sources.push_back(&source);
        }
    }

    /// @brief Drops @p node, which is being destroyed, from the recorded sources.
    /// @param node The node.
    void forget(Node& node) noexcept { std::erase(_sources, &node); }

    /// @brief Hands the recorded sources to the observer.
    /// @return The sources, deduplicated, in first-read order.
    [[nodiscard]] std::vector<Node*> takeSources() noexcept { return std::move(_sources); }

    /// @brief The enclosing frame.
    /// @return The frame that was innermost when this one was opened, or null.
    [[nodiscard]] TrackingFrame* previous() const noexcept { return _previous; }

private:
    RuntimeCore* _core;
    TrackingFrame* _previous;
    Node* _observer;
    std::vector<Node*> _sources;
};

/// @brief A graph node: a source (Signal), an observer (Effect), or both (Computed).
///
/// Non-copyable and non-movable: other nodes hold raw pointers to it, and its
/// destructor unlinks it in both directions.
class Node {
public:
    /// @param core The runtime core this node belongs to.
    explicit Node(std::shared_ptr<RuntimeCore> core) : _core{std::move(core)} { _core->nodeCreated(); }

    virtual ~Node() {
        for (Node* const source : _sources) {
            std::erase(source->_observers, this);
        }
        for (Node* const observer : _observers) {
            std::erase(observer->_sources, this);
        }
        _core->forget(*this);
        _core->nodeDestroyed();
    }

    Node(Node const&) = delete;
    Node& operator=(Node const&) = delete;
    Node(Node&&) = delete;
    Node& operator=(Node&&) = delete;

    /// @brief Push phase: raises this node to @p colour and marks its observers Check.
    ///
    /// An Effect is queued when it leaves Clean. A node already at least as stale stops the walk.
    /// @param colour Dirty for a direct observer of a change, Check for a transitive one.
    void markStale(Colour colour) {
        if (_colour >= colour) {
            return;
        }
        bool const wasClean = _colour == Colour::Clean;
        _colour = colour;
        if (wasClean && isEffect()) {
            _core->enqueue(*this);
        }
        for (Node* const observer : _observers) {
            observer->markStale(Colour::Check);
        }
    }

    /// @brief Pull phase: brings this node up to date, recomputing only if a source really changed.
    ///
    /// A recompute that throws leaves the node Clean (so later changes still reach it) and rethrows.
    void updateIfNecessary() {
        if (_colour == Colour::Check) {
            for (std::size_t i = 0; i < _sources.size(); ++i) {
                _sources.at(i)->updateIfNecessary();
                if (_colour == Colour::Dirty) {
                    break;
                }
            }
        }
        if (_colour == Colour::Dirty) {
            bool changed = false;
            try {
                changed = recompute();
            } catch (...) {
                _colour = Colour::Clean;
                throw;
            }
            if (changed) {
                markObserversStale(Colour::Dirty);
            }
        }
        _colour = Colour::Clean;
    }

    /// @brief Counts one run of this Effect in the flush numbered @p epoch.
    /// @param epoch The current flush's number.
    /// @return How many times it has run in that flush, this one included.
    [[nodiscard]] std::size_t countRun(std::uint64_t epoch) noexcept {
        if (_epoch != epoch) {
            _epoch = epoch;
            _runs = 0;
        }
        return ++_runs;
    }

    /// @brief Marks this node Clean without running it (a dropped queue entry).
    void settle() noexcept { _colour = Colour::Clean; }

protected:
    /// @brief The runtime core.
    /// @return The core this node belongs to.
    [[nodiscard]] RuntimeCore& core() const noexcept { return *_core; }

    /// @brief Registers this node as a source of the innermost tracked run, if any.
    void trackRead() const {
        if (TrackingFrame* const frame = _core->tracking(); frame != nullptr) {
            // Reading is logically const; the dependency link is bookkeeping.
            frame->add(const_cast<Node&>(*this));  // NOLINT(cppcoreguidelines-pro-type-const-cast)
        }
    }

    /// @brief Marks this node Dirty, so the next pull recomputes it.
    void forceDirty() noexcept { _colour = Colour::Dirty; }

    /// @brief Marks every observer of this node @p colour.
    /// @param colour The colour to raise them to.
    void markObserversStale(Colour colour) {
        for (Node* const observer : _observers) {
            observer->markStale(colour);
        }
    }

    /// @brief Replaces this node's sources with @p fresh, unlinking the ones no longer read.
    /// @param fresh The sources the latest run read.
    void adoptSources(std::vector<Node*> fresh) {
        for (Node* const old : _sources) {
            if (std::ranges::find(fresh, old) == fresh.end()) {
                std::erase(old->_observers, this);
            }
        }
        for (Node* const source : fresh) {
            if (std::ranges::find(_sources, source) == _sources.end()) {
                source->_observers.push_back(this);
            }
        }
        _sources = std::move(fresh);
    }

    /// @brief Adds @p fresh to this node's sources without dropping any: what a failed run keeps.
    /// @param fresh The sources the failed run read before it threw.
    void mergeSources(std::vector<Node*> fresh) {
        for (Node* const old : _sources) {
            if (std::ranges::find(fresh, old) == fresh.end()) {
                fresh.push_back(old);
            }
        }
        adoptSources(std::move(fresh));
    }

    /// @brief Brings the node's value up to date.
    /// @return True when observers must treat the value as changed.
    virtual bool recompute() = 0;

    /// @brief Whether this node is an Effect, which the flush queue runs.
    /// @return False unless overridden.
    [[nodiscard]] virtual bool isEffect() const noexcept { return false; }

private:
    std::shared_ptr<RuntimeCore> _core;
    std::vector<Node*> _sources;
    std::vector<Node*> _observers;
    std::uint64_t _epoch = 0;
    std::size_t _runs = 0;
    Colour _colour = Colour::Clean;
};

inline void RuntimeCore::flush() {
    _isFlushRequested = false;
    if (_detached || _queue.empty()) {
        return;
    }
    if (_widgetEventDepth != 0) {
        // A widget whose native handler is on the stack must not be torn down by a remount.
        report(site::kFlushInWidgetEvent);
        requestFlush();
        return;
    }
    _flushing = true;
    ++_flushEpoch;
    bool stopped = false;
    while (!_queue.empty()) {
        Node* const effect = _queue.front();
        _queue.pop_front();
        if (effect->countRun(_flushEpoch) > _maxEffectRunsPerFlush) {
            report(site::kWriteCycle);
            effect->settle();
            dropQueue();
            break;
        }
        try {
            effect->updateIfNecessary();
        } catch (...) {
            report(site::kEffectThrew);
            stopped = true;
            break;
        }
    }
    _flushing = false;
    if (stopped) {
        // What is still queued runs in a flush of its own.
        requestFlush();
    }
    if (_afterFlush) {
        _afterFlush();
    }
}

inline void RuntimeCore::forget(Node& node) noexcept {
    std::erase(_queue, &node);
    for (TrackingFrame* frame = _tracking; frame != nullptr; frame = frame->previous()) {
        frame->forget(node);
    }
}

inline void RuntimeCore::dropQueue() noexcept {
    for (Node* const node : _queue) {
        node->settle();
    }
    _queue.clear();
}

/// @brief Opens a batch for its lifetime.
class BatchScope {
public:
    /// @param core The runtime core.
    explicit BatchScope(RuntimeCore& core) noexcept : _core{&core} { core.beginBatch(); }
    ~BatchScope() {
        try {
            _core->endBatch();
        } catch (...) {  // NOLINT(bugprone-empty-catch)
            // Posting the flush failed to allocate. The queue is kept and the next write posts again.
        }
    }
    BatchScope(BatchScope const&) = delete;
    BatchScope& operator=(BatchScope const&) = delete;
    BatchScope(BatchScope&&) = delete;
    BatchScope& operator=(BatchScope&&) = delete;

private:
    RuntimeCore* _core;
};

/// @brief Marks a widget callback for its lifetime.
class WidgetEventScope {
public:
    /// @param core The runtime core.
    explicit WidgetEventScope(RuntimeCore& core) noexcept : _core{&core} { core.enterWidgetEvent(); }
    ~WidgetEventScope() { _core->leaveWidgetEvent(); }
    WidgetEventScope(WidgetEventScope const&) = delete;
    WidgetEventScope& operator=(WidgetEventScope const&) = delete;
    WidgetEventScope(WidgetEventScope&&) = delete;
    WidgetEventScope& operator=(WidgetEventScope&&) = delete;

private:
    RuntimeCore* _core;
};

}  // namespace morph::reactive::detail
```

- [ ] **Step 4: Write the Runtime**

Create `include/morph/reactive/runtime.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <cstddef>
#include <functional>
#include <memory>
#include <utility>

#include "../attributes.hpp"
#include "../core/executor.hpp"
#include "detail/graph.hpp"

/// @file
/// @brief `morph::reactive::Runtime`: the owner, batching and flush scheduling of a signal graph.
///
/// Specified in `docs/spec/reactive/signals.md`.

namespace morph::reactive {

/// @brief How a `Runtime` bounds and reports its flushes.
struct RuntimeOptions {
    /// @brief How many times one Effect may run in a single flush before the flush is treated as a
    ///        write cycle, reported, and stopped.
    std::size_t maxEffectRunsPerFlush = 100;
    /// @brief Called on the owner after every flush that ran at least one Effect; a frontend
    ///        schedules its redraw here. May be empty.
    std::function<void()> afterFlush;
};

/// @brief The owner of a signal graph: every node takes one, and every operation runs on its owner.
///
/// Writes are batched. When the outermost batch ends with effects queued, the
/// runtime posts **one** flush to the owner; no Effect ever runs inside `set()`.
/// Destroying a Runtime while nodes made from it are alive is reported
/// (`detail::site::kRuntimeOutlived`); the nodes stay valid and any flush does nothing.
class Runtime {
public:
    /// @param owner The executor the graph belongs to (the GUI or loop executor). Borrowed: it must
    ///        outlive the Runtime and every node made from it.
    /// @param options Flush bound and after-flush hook.
    explicit Runtime(exec::IExecutor& owner MORPH_LIFETIMEBOUND, RuntimeOptions options = {})
        : _core{std::make_shared<detail::RuntimeCore>(owner)} {
        _core->configure(options.maxEffectRunsPerFlush, std::move(options.afterFlush));
    }

    ~Runtime() {
        if (_core->liveNodes() != 0) {
            _core->report(detail::site::kRuntimeOutlived);
        }
        _core->detach();
    }

    Runtime(Runtime const&) = delete;
    Runtime& operator=(Runtime const&) = delete;
    Runtime(Runtime&&) = delete;
    Runtime& operator=(Runtime&&) = delete;

    /// @brief Runs @p fn as one batch: every write inside it is one flush.
    /// @tparam F A callable taking no arguments.
    /// @param fn The body. Writes made before it throws still flush.
    /// @return Whatever @p fn returns.
    template <typename F>
    decltype(auto) batch(F&& fn) {
        detail::BatchScope const scope{*_core};
        return std::invoke(std::forward<F>(fn));
    }

    /// @brief Runs a widget callback: one batch, during which a flush is refused and re-posted, so a
    ///        remount never destroys a widget whose native handler is on the stack.
    /// @tparam F A callable taking no arguments.
    /// @param fn The callback.
    /// @return Whatever @p fn returns.
    template <typename F>
    decltype(auto) widgetEvent(F&& fn) {
        detail::WidgetEventScope const event{*_core};
        return batch(std::forward<F>(fn));
    }

    /// @brief Runs @p fn with tracking off: reads inside it subscribe nothing.
    /// @tparam F A callable taking no arguments.
    /// @param fn The body.
    /// @return Whatever @p fn returns.
    template <typename F>
    decltype(auto) untracked(F&& fn) const {
        detail::TrackingFrame const frame{*_core, nullptr};
        return std::invoke(std::forward<F>(fn));
    }

    /// @brief The owner executor.
    /// @return The executor passed at construction.
    [[nodiscard]] exec::IExecutor& owner() const noexcept { return _core->owner(); }

    /// @brief Whether a flush is posted and has not run yet.
    /// @return True between the first write that queued an Effect and the flush.
    [[nodiscard]] bool isFlushRequested() const noexcept { return _core->isFlushRequested(); }

    /// @brief The shared core every node holds.
    /// @return The core.
    [[nodiscard]] std::shared_ptr<detail::RuntimeCore> const& core() const noexcept { return _core; }

private:
    std::shared_ptr<detail::RuntimeCore> _core;
};

}  // namespace morph::reactive
```

- [ ] **Step 5: Write `Signal` and `Effect`**

Create `include/morph/reactive/signal.hpp` (Task 2 adds `Computed` to this file):

```cpp
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <concepts>
#include <functional>
#include <type_traits>
#include <utility>

#include "detail/graph.hpp"
#include "runtime.hpp"

/// @file
/// @brief `Signal<T>`, `Computed<T>` and `Effect`: the nodes of a reactive graph.
///
/// Specified in `docs/spec/reactive/signals.md`.

namespace morph::reactive {

/// @brief A source value. Reading it with `get()` inside a tracked run subscribes that run.
///
/// `set()` skips a value equal to the current one when `T` is equality-comparable; `mutate()`
/// always notifies. Writes are refused, and reported, off the owner or inside a Computed.
/// @tparam T The value type; move-constructible and move-assignable.
template <typename T>
class Signal final : public detail::Node {
public:
    /// @brief Constructs a value-initialised signal.
    /// @param runtime The runtime this signal belongs to.
    explicit Signal(Runtime& runtime)
        requires std::default_initializable<T>
        : Node{runtime.core()}, _value{} {}

    /// @brief Constructs a signal holding @p initial.
    /// @param runtime The runtime this signal belongs to.
    /// @param initial The initial value.
    Signal(Runtime& runtime, T initial) : Node{runtime.core()}, _value{std::move(initial)} {}

    ~Signal() override = default;
    Signal(Signal const&) = delete;
    Signal& operator=(Signal const&) = delete;
    Signal(Signal&&) = delete;
    Signal& operator=(Signal&&) = delete;

    /// @brief Reads the value, subscribing the innermost tracked run.
    /// @return The current value; valid until the next write.
    [[nodiscard]] T const& get() const {
        static_cast<void>(core().checkOwner());
        trackRead();
        return _value;
    }

    /// @brief Reads the value without subscribing anything.
    /// @return The current value; valid until the next write.
    [[nodiscard]] T const& peek() const noexcept { return _value; }

    /// @brief Replaces the value and notifies observers, unless it equals the current one.
    /// @param value The new value.
    void set(T value) {
        if (!writable()) {
            return;
        }
        if constexpr (std::equality_comparable<T>) {
            if (_value == value) {
                return;
            }
        }
        _value = std::move(value);
        notify();
    }

    /// @brief Changes the value in place and always notifies observers.
    /// @tparam F A callable taking `T&`.
    /// @param fn The mutation.
    template <typename F>
        requires std::invocable<F&, T&>
    void mutate(F&& fn) {
        if (!writable()) {
            return;
        }
        std::invoke(fn, _value);
        notify();
    }

private:
    [[nodiscard]] bool writable() const noexcept {
        if (!core().checkOwner()) {
            return false;
        }
        if (core().isComputing()) {
            core().report(detail::site::kSetInComputed);
            return false;
        }
        return true;
    }

    void notify() {
        detail::BatchScope const batch{core()};
        markObserversStale(detail::Colour::Dirty);
    }

    bool recompute() override { return false; }

    T _value;
};

/// @brief A side effect: runs once on construction, then in a flush after any source it read changed.
///
/// An exception escaping it is a defect: reported (`detail::site::kEffectThrew`), and the flush stops.
class Effect final : public detail::Node {
public:
    /// @tparam F A callable taking no arguments.
    /// @param runtime The runtime this effect belongs to.
    /// @param fn The body; every signal it reads becomes a dependency.
    template <typename F>
        requires std::invocable<F&>
    Effect(Runtime& runtime, F fn) : Node{runtime.core()}, _fn{std::move(fn)} {
        forceDirty();
        if (!core().checkOwner()) {
            return;
        }
        try {
            updateIfNecessary();
        } catch (...) {
            core().report(detail::site::kEffectThrew);
        }
    }

    ~Effect() override = default;
    Effect(Effect const&) = delete;
    Effect& operator=(Effect const&) = delete;
    Effect(Effect&&) = delete;
    Effect& operator=(Effect&&) = delete;

private:
    [[nodiscard]] bool isEffect() const noexcept override { return true; }

    bool recompute() override {
        detail::TrackingFrame frame{core(), this};
        try {
            std::invoke(_fn);
        } catch (...) {
            mergeSources(frame.takeSources());
            throw;
        }
        adoptSources(frame.takeSources());
        return false;
    }

    std::function<void()> _fn;
};

}  // namespace morph::reactive
```

- [ ] **Step 6: Register the headers**

In `CMakeLists.txt`, in the `FILE_SET HEADERS` block of `target_sources(morph …)`, add after
`include/morph/util/datetime.hpp`:

```cmake
        include/morph/reactive/runtime.hpp
        include/morph/reactive/signal.hpp
```

and in the `FILE_SET morph_detail_headers` block, after `include/morph/core/detail/owner_affinity.hpp`:

```cmake
        include/morph/reactive/detail/graph.hpp
```

- [ ] **Step 7: Run the tests to verify they pass**

Run: `cmake --build build/reactive --target morph_tests && ./build/reactive/tests/morph_tests "[reactive]"`
Expected: PASS, `All tests passed (… assertions in 15 test cases)`.

Then mutate once to prove the scheduling tests measure something: in `RuntimeCore::requestFlush`, delete
`_isFlushRequested ||` from the guard, rebuild, and run again. Expected: FAIL in "exactly one post per
idle-to-pending transition". Restore the line.

- [ ] **Step 8: Commit**

```bash
git add include/morph/reactive tests/test_reactive_signal.cpp tests/CMakeLists.txt CMakeLists.txt
git commit -m "wip(reactive): signal graph core, Runtime, Signal and Effect

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 2: `Computed<T>` and glitch-freedom

**Files:**
- Modify: `include/morph/reactive/signal.hpp` — add `Computed<T>` between `Signal` and `Effect`
- Modify: `tests/CMakeLists.txt` — add `test_reactive_computed.cpp` after `test_reactive_signal.cpp`
- Test: `tests/test_reactive_computed.cpp`

**Interfaces:**
- Consumes: everything Task 1 produces.
- Produces: `reactive::Computed<T>(Runtime&, F)` with `get() const -> T const&` (tracked; rethrows a failed
  computation, retries on the next read) and `peek() const -> T const&` (untracked).

- [ ] **Step 1: Write the failing test**

Create `tests/test_reactive_computed.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/signal.hpp>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#include "test_support.hpp"

namespace {

using morph::reactive::Computed;
using morph::reactive::Effect;
using morph::reactive::Runtime;
using morph::reactive::Signal;
using Owner = morph::testing::StepExecutor;

class NoDefault {
public:
    explicit NoDefault(int value) : _value{value} {}
    [[nodiscard]] int value() const { return _value; }
    bool operator==(NoDefault const&) const = default;

private:
    int _value;
};

struct Opaque {
    int value = 0;
};

}  // namespace

static_assert(!std::is_copy_constructible_v<Computed<int>> && !std::is_move_constructible_v<Computed<int>>);

TEST_CASE("reactive::Computed: lazy, cached, recomputed only after a source changes", "[reactive]") {
    Owner owner;
    Runtime runtime{owner};
    Signal<int> base{runtime, 2};
    int calls = 0;
    Computed<int> twice{runtime, [&] {
                            ++calls;
                            return base.get() * 2;
                        }};
    CHECK(calls == 0);
    CHECK(twice.get() == 4);
    CHECK(twice.get() == 4);
    CHECK(calls == 1);
    base.set(3);
    CHECK(twice.get() == 6);
    CHECK(calls == 2);
}

TEST_CASE("reactive::Computed: a diamond is seen consistently, once per change", "[reactive]") {
    Owner owner;
    Runtime runtime{owner};
    Signal<int> base{runtime, 1};
    Computed<int> twice{runtime, [&] { return base.get() * 2; }};
    Computed<int> thrice{runtime, [&] { return base.get() * 3; }};
    Computed<int> sum{runtime, [&] { return twice.get() + thrice.get(); }};
    std::vector<int> seen;
    Effect const log{runtime, [&] { seen.push_back(sum.get()); }};
    base.set(2);
    owner.runAll();
    CHECK(seen == std::vector{5, 10});
}

TEST_CASE("reactive::Computed: an Effect reading A before Computed(A) never sees them disagree", "[reactive]") {
    Owner owner;
    Runtime runtime{owner};
    Signal<int> base{runtime, 1};
    Computed<int> twice{runtime, [&] { return base.get() * 2; }};
    std::vector<std::pair<int, int>> seen;
    Effect const log{runtime, [&] {
                         int const raw = base.get();
                         seen.emplace_back(raw, twice.get());
                     }};
    base.set(5);
    owner.runAll();
    REQUIRE(seen.size() == 2);
    for (auto const& [raw, doubled] : seen) {
        CHECK(doubled == raw * 2);
    }
}

TEST_CASE("reactive::Computed: an unchanged result stops propagation", "[reactive]") {
    Owner owner;
    Runtime runtime{owner};
    Signal<int> base{runtime, 1};
    Computed<bool> positive{runtime, [&] { return base.get() > 0; }};
    int runs = 0;
    Effect const watch{runtime, [&] {
                           ++runs;
                           static_cast<void>(positive.get());
                       }};
    base.set(2);
    owner.runAll();
    CHECK(runs == 1);
    base.set(-1);
    owner.runAll();
    CHECK(runs == 2);
}

TEST_CASE("reactive::Computed: a non-comparable result always propagates", "[reactive]") {
    Owner owner;
    Runtime runtime{owner};
    Signal<int> base{runtime, 1};
    Computed<Opaque> wrapped{runtime, [&] { return Opaque{base.get() > 0 ? 1 : 0}; }};
    int runs = 0;
    Effect const watch{runtime, [&] {
                           ++runs;
                           static_cast<void>(wrapped.get());
                       }};
    base.set(2);
    owner.runAll();
    CHECK(runs == 2);
}

TEST_CASE("reactive::Computed: T need not be default-constructible", "[reactive]") {
    Owner owner;
    Runtime runtime{owner};
    Signal<int> base{runtime, 7};
    Computed<NoDefault> boxed{runtime, [&] { return NoDefault{base.get()}; }};
    CHECK(boxed.get().value() == 7);
}

TEST_CASE("reactive::Computed: a throw reaches the reader, the next read retries", "[reactive]") {
    Owner owner;
    Runtime runtime{owner};
    Signal<int> divisor{runtime, 0};
    int calls = 0;
    Computed<int> quotient{runtime, [&] {
                               ++calls;
                               if (divisor.get() == 0) {
                                   throw std::domain_error{"division by zero"};
                               }
                               return 100 / divisor.get();
                           }};
    CHECK_THROWS_AS(quotient.get(), std::domain_error);
    CHECK_THROWS_AS(quotient.get(), std::domain_error);
    CHECK(calls == 2);
    CHECK(runtime.core()->tracking() == nullptr);
    divisor.set(4);
    CHECK(quotient.get() == 25);
}

TEST_CASE("reactive::Computed: a computed that failed still hears about later changes", "[reactive]") {
    Owner owner;
    Runtime runtime{owner};
    Signal<int> divisor{runtime, 0};
    Computed<int> quotient{runtime, [&] {
                               if (divisor.get() == 0) {
                                   throw std::domain_error{"division by zero"};
                               }
                               return 100 / divisor.get();
                           }};
    int seen = -1;
    Signal<bool> ready{runtime, false};
    Effect const watch{runtime, [&] {
                           if (ready.get()) {
                               seen = quotient.get();
                           }
                       }};
    CHECK_THROWS_AS(quotient.get(), std::domain_error);
    divisor.set(5);
    ready.set(true);
    owner.runAll();
    CHECK(seen == 20);
}

TEST_CASE("reactive::Computed: peek does not subscribe", "[reactive]") {
    Owner owner;
    Runtime runtime{owner};
    Signal<int> base{runtime, 1};
    Computed<int> twice{runtime, [&] { return base.get() * 2; }};
    int runs = 0;
    Effect const watch{runtime, [&] {
                           ++runs;
                           static_cast<void>(twice.peek());
                       }};
    base.set(2);
    CHECK(owner.pending() == 0);
    CHECK(twice.peek() == 4);
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build/reactive --target morph_tests`
Expected: FAIL — `no template named 'Computed' in namespace 'morph::reactive'`.

- [ ] **Step 3: Implement `Computed`**

In `include/morph/reactive/signal.hpp`, add `#include <exception>`, `#include <optional>` and
`#include <stdexcept>` to the includes, and insert between `Signal` and `Effect`:

```cpp
/// @brief A derived value: computed lazily on first read, cached, and recomputed only after a
///        source it read really changed.
///
/// Equality-gated when `T` is equality-comparable: an unchanged result stops propagation. A
/// computation that throws rethrows to the reader, and the next read retries it. Writing a signal
/// inside the computation, or reading the Computed from inside itself, is refused and reported.
/// @tparam T The value type; move-constructible. It need not be default-constructible.
template <typename T>
class Computed final : public detail::Node {
public:
    /// @tparam F A callable taking no arguments and returning something convertible to `T`.
    /// @param runtime The runtime this computed belongs to.
    /// @param fn The computation; every signal or computed it reads becomes a dependency.
    template <typename F>
        requires std::invocable<F&> && std::convertible_to<std::invoke_result_t<F&>, T>
    Computed(Runtime& runtime, F fn) : Node{runtime.core()}, _fn{std::move(fn)} {
        forceDirty();
    }

    ~Computed() override = default;
    Computed(Computed const&) = delete;
    Computed& operator=(Computed const&) = delete;
    Computed(Computed&&) = delete;
    Computed& operator=(Computed&&) = delete;

    /// @brief Reads the value, bringing it up to date first, and subscribes the innermost tracked run.
    /// @return The current value; valid until the next recomputation.
    /// @throws Whatever the computation throws; `std::logic_error` when read from inside itself.
    [[nodiscard]] T const& get() const {
        // Reading is logically const; the cache and the dependency links are bookkeeping.
        auto& self = const_cast<Computed&>(*this);  // NOLINT(cppcoreguidelines-pro-type-const-cast)
        static_cast<void>(core().checkOwner());
        if (_computing) {
            core().report(detail::site::kComputedReadsItself);
            throw std::logic_error{"morph::reactive: a Computed read itself"};
        }
        trackRead();
        if (_error != nullptr) {
            self.forceDirty();
        }
        self.updateIfNecessary();
        // Always engaged here: a node starts Dirty, and a failed computation rethrows above.
        return _value.value();
    }

    /// @brief Reads the value, bringing it up to date first, without subscribing anything.
    /// @return The current value; valid until the next recomputation.
    /// @throws Whatever the computation throws.
    [[nodiscard]] T const& peek() const {
        detail::TrackingFrame const untracked{core(), nullptr};
        return get();
    }

private:
    class Computing {
    public:
        explicit Computing(Computed& self) noexcept : _self{&self} {
            self._computing = true;
            self.core().enterComputed();
        }
        ~Computing() {
            _self->_computing = false;
            _self->core().leaveComputed();
        }
        Computing(Computing const&) = delete;
        Computing& operator=(Computing const&) = delete;
        Computing(Computing&&) = delete;
        Computing& operator=(Computing&&) = delete;

    private:
        Computed* _self;
    };

    bool recompute() override {
        Computing const computing{*this};
        detail::TrackingFrame frame{core(), this};
        std::optional<T> fresh;
        try {
            fresh.emplace(std::invoke(_fn));
        } catch (...) {
            mergeSources(frame.takeSources());
            _error = std::current_exception();
            throw;
        }
        adoptSources(frame.takeSources());
        _error = nullptr;
        if constexpr (std::equality_comparable<T>) {
            if (_value.has_value() && *_value == *fresh) {
                return false;
            }
        }
        _value.emplace(std::move(*fresh));
        return true;
    }

    std::function<T()> _fn;
    std::optional<T> _value;
    std::exception_ptr _error;
    bool _computing = false;
};
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build/reactive --target morph_tests && ./build/reactive/tests/morph_tests "[reactive]"`
Expected: PASS.

Mutation check: in `Node::updateIfNecessary`, replace the Check walk's body with nothing (so a Check node
never pulls its sources) and rebuild. Expected: FAIL in "a diamond is seen consistently" or "an unchanged result
stops propagation". Restore.

- [ ] **Step 5: Commit**

```bash
git add include/morph/reactive/signal.hpp tests/test_reactive_computed.cpp tests/CMakeLists.txt
git commit -m "wip(reactive): Computed, glitch-free and equality-gated

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 3: Misuse is reported, then refused

The reporting paths were written in Tasks 1–2; this task pins each one with a test that installs the owner
probe, and proves each test can fail.

**Files:**
- Modify: `tests/CMakeLists.txt` — add `test_reactive_misuse.cpp` after `test_reactive_computed.cpp`
- Test: `tests/test_reactive_misuse.cpp`

**Interfaces:**
- Consumes: `detail::site::*`, `Runtime`, `Signal`, `Computed`, `Effect`; `morph::testing::OwnerProbeRecorder`
  (`tests/owner_probe_recorder.hpp`, constructed with `owner.coreExecutor()`, queried with `count(site)`).
- Produces: nothing new.

- [ ] **Step 1: Write the tests**

Create `tests/test_reactive_misuse.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <functional>
#include <memory>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/signal.hpp>
#include <stdexcept>
#include <thread>

#include "owner_probe_recorder.hpp"
#include "test_support.hpp"

namespace {

using morph::reactive::Computed;
using morph::reactive::Effect;
using morph::reactive::Runtime;
using morph::reactive::RuntimeOptions;
using morph::reactive::Signal;
using Owner = morph::testing::StepExecutor;
using Probe = morph::testing::OwnerProbeRecorder;
namespace site = morph::reactive::detail::site;

}  // namespace

TEST_CASE("reactive misuse: an exception escaping an Effect stops that flush", "[reactive][misuse]") {
    Owner owner;
    Probe const probe{owner.coreExecutor()};
    Runtime runtime{owner};
    Signal<int> value{runtime, 0};
    Effect const thrower{runtime, [&] {
                             if (value.get() == 1) {
                                 throw std::runtime_error{"boom"};
                             }
                         }};
    int laterRuns = 0;
    Effect const later{runtime, [&] {
                           static_cast<void>(value.get());
                           ++laterRuns;
                       }};
    value.set(1);
    REQUIRE(owner.runOne());
    CHECK(probe.count(site::kEffectThrew) == 1);
    CHECK(laterRuns == 1);
    owner.runAll();  // what was still queued runs in a flush of its own
    CHECK(laterRuns == 2);
}

TEST_CASE("reactive misuse: an Effect that throws on construction is reported", "[reactive][misuse]") {
    Owner owner;
    Probe const probe{owner.coreExecutor()};
    Runtime runtime{owner};
    Effect const thrower{runtime, [] { throw std::runtime_error{"boom"}; }};
    CHECK(probe.count(site::kEffectThrew) == 1);
}

TEST_CASE("reactive misuse: a write cycle is reported and the flush stops", "[reactive][misuse]") {
    Owner owner;
    Probe const probe{owner.coreExecutor()};
    Runtime runtime{owner, RuntimeOptions{.maxEffectRunsPerFlush = 10}};
    Signal<int> ping{runtime, 0};
    Signal<int> pong{runtime, 0};
    Effect const forward{runtime, [&] { pong.set(ping.get() + 1); }};
    Effect const backward{runtime, [&] { ping.set(pong.get() + 1); }};
    owner.runAll();
    CHECK(probe.count(site::kWriteCycle) == 1);
    CHECK(owner.pending() == 0);
}

TEST_CASE("reactive misuse: set() inside a Computed is dropped", "[reactive][misuse]") {
    Owner owner;
    Probe const probe{owner.coreExecutor()};
    Runtime runtime{owner};
    Signal<int> base{runtime, 0};
    Signal<int> other{runtime, 0};
    Computed<int> sneaky{runtime, [&] {
                             other.set(7);
                             return base.get();
                         }};
    CHECK(sneaky.get() == 0);
    CHECK(other.peek() == 0);
    CHECK(probe.count(site::kSetInComputed) == 1);
}

TEST_CASE("reactive misuse: a Computed reading itself throws logic_error", "[reactive][misuse]") {
    Owner owner;
    Probe const probe{owner.coreExecutor()};
    Runtime runtime{owner};
    std::function<int()> body;
    Computed<int> loop{runtime, [&] { return body(); }};
    body = [&] { return loop.get() + 1; };
    CHECK_THROWS_AS(loop.get(), std::logic_error);
    CHECK(probe.count(site::kComputedReadsItself) == 1);
}

TEST_CASE("reactive misuse: a write from a foreign thread is dropped", "[reactive][misuse]") {
    Owner owner;
    Probe const probe{owner.coreExecutor()};
    Runtime runtime{owner};
    Signal<int> value{runtime, 0};
    std::thread{[&] { value.set(1); }}.join();
    CHECK(value.peek() == 0);
    CHECK(probe.count(site::kOffOwner) == 1);
}

TEST_CASE("reactive misuse: a Runtime destroyed before its nodes", "[reactive][misuse]") {
    Owner owner;
    Probe const probe{owner.coreExecutor()};
    auto runtime = std::make_unique<Runtime>(owner);
    Signal<int> value{*runtime, 0};
    int runs = 0;
    Effect const watch{*runtime, [&] {
                           static_cast<void>(value.get());
                           ++runs;
                       }};
    value.set(1);
    runtime.reset();
    CHECK(probe.count(site::kRuntimeOutlived) == 1);
    owner.runAll();  // the posted flush finds the core detached
    CHECK(runs == 1);
}

TEST_CASE("reactive misuse: a flush inside a widget event is re-posted", "[reactive][misuse]") {
    Owner owner;
    Probe const probe{owner.coreExecutor()};
    Runtime runtime{owner};
    Signal<int> value{runtime, 0};
    int runs = 0;
    Effect const watch{runtime, [&] {
                           static_cast<void>(value.get());
                           ++runs;
                       }};
    value.set(1);
    runtime.widgetEvent([&] { REQUIRE(owner.runOne()); });
    CHECK(probe.count(site::kFlushInWidgetEvent) == 1);
    CHECK(runs == 1);
    owner.runAll();
    CHECK(runs == 2);
}
```

- [ ] **Step 2: Run the tests**

Run: `cmake --build build/reactive --target morph_tests && ./build/reactive/tests/morph_tests "[misuse]"`
Expected: PASS — the behaviour landed in Tasks 1–2.

- [ ] **Step 3: Prove each test can fail**

Make each change, rebuild, run `"[misuse]"`, confirm the named test fails, then restore:

| Change | Test that must fail |
|---|---|
| In `RuntimeCore::flush`, replace the `catch (...)` body with `report(site::kEffectThrew);` (no `stopped`/`break`) | "an exception escaping an Effect stops that flush" |
| In `RuntimeCore::flush`, delete the `countRun` block | "a write cycle is reported" — the flush loops forever, so run it as `timeout 30 ./build/reactive/tests/morph_tests "a write cycle*"` and expect a non-zero exit |
| In `Signal::writable`, delete the `isComputing()` block | "set() inside a Computed is dropped" |
| In `Computed::get`, delete the `_computing` block | "a Computed reading itself" (stack overflow) |
| In `Signal::writable`, replace `if (!core().checkOwner())` with `if (false)` | "a write from a foreign thread is dropped" |
| In `~Runtime`, delete `_core->detach();` | "a Runtime destroyed before its nodes" |

- [ ] **Step 4: Commit**

```bash
git add tests/test_reactive_misuse.cpp tests/CMakeLists.txt
git commit -m "wip(reactive): pin every reported-then-refused misuse

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 4: `Scope`

**Files:**
- Create: `include/morph/reactive/scope.hpp`
- Modify: `CMakeLists.txt` — add `include/morph/reactive/scope.hpp` after `include/morph/reactive/signal.hpp`
- Modify: `tests/CMakeLists.txt` — add `test_reactive_scope.cpp` after `test_reactive_misuse.cpp`
- Test: `tests/test_reactive_scope.cpp`

**Interfaces:**
- Consumes: `Runtime`, `Signal`, `Computed`, `Effect`.
- Produces: `reactive::Scope(Runtime&)`: `make<T>(Args&&...) -> T&`, `adopt<T>(std::unique_ptr<T>) -> T&`,
  `effect(F) -> Effect&`, `computed(F) -> Computed<R>&`, `clear()`, `size()`, `runtime()`. Destroys owned
  objects in reverse creation order. Part 2 mounts each Switch case and ForEach row into one.

- [ ] **Step 1: Write the failing test**

Create `tests/test_reactive_scope.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/scope.hpp>
#include <morph/reactive/signal.hpp>
#include <string>
#include <vector>

#include "test_support.hpp"

namespace {

using morph::reactive::Runtime;
using morph::reactive::Scope;
using morph::reactive::Signal;
using Owner = morph::testing::StepExecutor;

class Witness {
public:
    Witness(std::vector<std::string>& log, std::string name) : _log{&log}, _name{std::move(name)} {}
    ~Witness() { _log->push_back(_name); }
    Witness(Witness const&) = delete;
    Witness& operator=(Witness const&) = delete;
    Witness(Witness&&) = delete;
    Witness& operator=(Witness&&) = delete;

private:
    std::vector<std::string>* _log;
    std::string _name;
};

}  // namespace

TEST_CASE("reactive::Scope: destroys what it owns in reverse creation order", "[reactive]") {
    Owner owner;
    Runtime runtime{owner};
    std::vector<std::string> log;
    {
        Scope scope{runtime};
        scope.make<Witness>(log, "first");
        scope.adopt(std::make_unique<Witness>(log, "second"));
        scope.make<Witness>(log, "third");
        CHECK(scope.size() == 3);
    }
    CHECK(log == std::vector<std::string>{"third", "second", "first"});
}

TEST_CASE("reactive::Scope: an effect it owns stops when the scope is cleared", "[reactive]") {
    Owner owner;
    Runtime runtime{owner};
    Signal<int> value{runtime, 0};
    Scope scope{runtime};
    int runs = 0;
    scope.effect([&] {
        static_cast<void>(value.get());
        ++runs;
    });
    auto& doubled = scope.computed([&] { return value.get() * 2; });
    CHECK(doubled.get() == 0);
    scope.clear();
    CHECK(scope.size() == 0);
    value.set(1);
    CHECK(owner.pending() == 0);
    CHECK(runs == 1);
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build/reactive --target morph_tests`
Expected: FAIL — `'morph/reactive/scope.hpp' file not found`.

- [ ] **Step 3: Implement `Scope`**

Create `include/morph/reactive/scope.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <cstddef>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

#include "../attributes.hpp"
#include "runtime.hpp"
#include "signal.hpp"

/// @file
/// @brief `morph::reactive::Scope`: owns reactive nodes and objects, and destroys them in reverse.
///
/// Specified in `docs/spec/reactive/signals.md`.

namespace morph::reactive {

/// @brief Owns effects, computeds and adopted objects; destroys them in reverse creation order.
///
/// A mounted view, a Switch case and a ForEach row each own one, so tearing one down stops every
/// binding it made before anything those bindings point at goes away.
class Scope {
public:
    /// @param runtime The runtime the nodes made here belong to. Borrowed: it must outlive the scope.
    explicit Scope(Runtime& runtime MORPH_LIFETIMEBOUND) noexcept : _rt{&runtime} {}

    ~Scope() { clear(); }
    Scope(Scope const&) = delete;
    Scope& operator=(Scope const&) = delete;
    Scope(Scope&&) = delete;
    Scope& operator=(Scope&&) = delete;

    /// @brief Constructs a `T` from @p args and owns it.
    /// @tparam T The object type.
    /// @tparam Args Constructor argument types.
    /// @param args Forwarded to `T`'s constructor.
    /// @return The new object, alive until the scope is cleared or destroyed.
    template <typename T, typename... Args>
    T& make(Args&&... args) {
        return adopt(std::make_unique<T>(std::forward<Args>(args)...));
    }

    /// @brief Takes ownership of @p owned.
    /// @tparam T The object type.
    /// @param owned The object; must not be null.
    /// @return The object, alive until the scope is cleared or destroyed.
    template <typename T>
    T& adopt(std::unique_ptr<T> owned) {
        _owned.reserve(_owned.size() + 1);
        T& ref = *owned;
        _owned.emplace_back(owned.release(), [](void* ptr) noexcept { std::default_delete<T>{}(static_cast<T*>(ptr)); });
        return ref;
    }

    /// @brief Makes an Effect owned by this scope.
    /// @tparam F A callable taking no arguments.
    /// @param fn The effect body.
    /// @return The Effect.
    template <typename F>
    Effect& effect(F&& fn) {
        return make<Effect>(*_rt, std::forward<F>(fn));
    }

    /// @brief Makes a Computed owned by this scope; its type is what @p fn returns.
    /// @tparam F A callable taking no arguments.
    /// @param fn The computation.
    /// @return The Computed.
    template <typename F>
    auto& computed(F&& fn) {
        using Value = std::remove_cvref_t<std::invoke_result_t<F&>>;
        return make<Computed<Value>>(*_rt, std::forward<F>(fn));
    }

    /// @brief Destroys everything owned, newest first.
    void clear() noexcept {
        while (!_owned.empty()) {
            _owned.pop_back();
        }
    }

    /// @brief How many objects the scope owns.
    /// @return The count.
    [[nodiscard]] std::size_t size() const noexcept { return _owned.size(); }

    /// @brief The runtime.
    /// @return The runtime passed at construction.
    [[nodiscard]] Runtime& runtime() const noexcept { return *_rt; }

private:
    Runtime* _rt;
    std::vector<std::unique_ptr<void, void (*)(void*)>> _owned;
};

}  // namespace morph::reactive
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build/reactive --target morph_tests && ./build/reactive/tests/morph_tests "[reactive]"`
Expected: PASS. Mutation check: make `clear()` destroy front-first
(`_owned.erase(_owned.begin())`); expected FAIL in "destroys what it owns in reverse creation order". Restore.

- [ ] **Step 5: Commit**

```bash
git add include/morph/reactive/scope.hpp tests/test_reactive_scope.cpp tests/CMakeLists.txt CMakeLists.txt
git commit -m "wip(reactive): Scope owns nodes and destroys them in reverse

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 5: `Store<ViewState, Msg>`, `ExhaustiveUpdate` and `request()`

**Files:**
- Create: `include/morph/reactive/store.hpp`
- Modify: `CMakeLists.txt` — add `include/morph/reactive/store.hpp` after `include/morph/reactive/scope.hpp`
- Modify: `tests/CMakeLists.txt` — add `test_reactive_store.cpp` after `test_reactive_scope.cpp`
- Test: `tests/test_reactive_store.cpp`

**Interfaces:**
- Consumes: `Runtime` (`batch`, `untracked`, `core()`), `detail::site::kSendInUpdate`;
  `morph::bridge::BridgeHandler<Model, Sharing>::execute(Action) -> Completion<ActionTraits<Action>::Result>`
  (`core/bridge.hpp`); `Completion<T>::then(CallbackScope const&, …)` / `onError(CallbackScope const&, …)`;
  `morph::model::ActionTraits<A>::Result` (`core/registry.hpp`).
- Produces:
  - `reactive::ExhaustiveUpdate<Update, ViewState, Msg>` — concept: `Update&` invocable as
    `(ViewState&, Alt const&)` for every alternative of the `std::variant` `Msg`.
  - `reactive::Store<ViewState, Msg>(Runtime&, Init, Update)` — `send(Msg)`, `state() -> ViewState const&`,
    `action(Msg) -> std::function<void()>`.
  - `reactive::request(Store&, BridgeHandler&, CallbackScope const&, Action, ToMsg, ToFailMsg)`.

- [ ] **Step 1: Write the failing test**

Create `tests/test_reactive_store.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <exception>
#include <memory>
#include <morph/core/bridge.hpp>
#include <morph/core/callback_scope.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/registry.hpp>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/signal.hpp>
#include <morph/reactive/store.hpp>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <variant>

#include "owner_probe_recorder.hpp"
#include "test_support.hpp"

namespace {

using morph::reactive::Effect;
using morph::reactive::ExhaustiveUpdate;
using morph::reactive::Runtime;
using morph::reactive::Signal;
using morph::reactive::Store;
using Owner = morph::testing::StepExecutor;

template <typename... Fs>
struct Overloaded : Fs... {
    using Fs::operator()...;
};

struct Counter {
    Signal<int> count;
    Signal<std::string> label;
    Signal<bool> armed;
};

struct Inc {};
struct Rename {
    std::string to;
};
struct Arm {};
using CounterMsg = std::variant<Inc, Rename, Arm>;

Counter initCounter(Runtime& runtime) {
    return Counter{.count{runtime, 0}, .label{runtime, "idle"}, .armed{runtime, false}};
}

auto const updateCounter = Overloaded{
    [](Counter& state, Inc const&) { state.count.set(state.count.peek() + 1); },
    [](Counter& state, Rename const& msg) { state.label.set(msg.to); },
    [](Counter& state, Arm const&) {
        state.count.set(10);
        state.label.set("armed");
        state.armed.set(true);
    },
};

auto const partialUpdate = Overloaded{
    [](Counter& state, Inc const&) { state.count.set(state.count.peek() + 1); },
    [](Counter& state, Rename const& msg) { state.label.set(msg.to); },
};

using InitFn = Counter (*)(Runtime&);

}  // namespace

static_assert(ExhaustiveUpdate<decltype(updateCounter), Counter, CounterMsg>);
static_assert(!ExhaustiveUpdate<decltype(partialUpdate), Counter, CounterMsg>);
static_assert(std::is_constructible_v<Store<Counter, CounterMsg>, Runtime&, InitFn, decltype(updateCounter)>);
static_assert(!std::is_constructible_v<Store<Counter, CounterMsg>, Runtime&, InitFn, decltype(partialUpdate)>);

TEST_CASE("reactive::Store: send applies the update for that alternative", "[reactive][store]") {
    Owner owner;
    Runtime runtime{owner};
    Store<Counter, CounterMsg> store{runtime, initCounter, updateCounter};
    store.send(Inc{});
    store.send(Rename{"two"});
    CHECK(store.state().count.peek() == 1);
    CHECK(store.state().label.peek() == "two");
}

TEST_CASE("reactive::Store: a three-field update is one flush", "[reactive][store]") {
    Owner owner;
    Runtime runtime{owner};
    Store<Counter, CounterMsg> store{runtime, initCounter, updateCounter};
    int runs = 0;
    Effect const watch{runtime, [&] {
                           static_cast<void>(store.state().count.get());
                           static_cast<void>(store.state().label.get());
                           static_cast<void>(store.state().armed.get());
                           ++runs;
                       }};
    store.send(Arm{});
    CHECK(owner.pending() == 1);
    owner.runAll();
    CHECK(runs == 2);
}

TEST_CASE("reactive::Store: reading one field tracks exactly that field", "[reactive][store]") {
    Owner owner;
    Runtime runtime{owner};
    Store<Counter, CounterMsg> store{runtime, initCounter, updateCounter};
    Effect const watch{runtime, [&] { static_cast<void>(store.state().label.get()); }};
    store.send(Inc{});
    CHECK(owner.pending() == 0);
}

TEST_CASE("reactive::Store: action() sends its message each time it is called", "[reactive][store]") {
    Owner owner;
    Runtime runtime{owner};
    Store<Counter, CounterMsg> store{runtime, initCounter, updateCounter};
    auto const inc = store.action(Inc{});
    inc();
    inc();
    CHECK(store.state().count.peek() == 2);
}

TEST_CASE("reactive::Store: send() inside an update is dropped and reported", "[reactive][store][misuse]") {
    Owner owner;
    morph::testing::OwnerProbeRecorder const probe{owner.coreExecutor()};
    Runtime runtime{owner};
    Store<Counter, CounterMsg>* self = nullptr;
    auto const reentrant = Overloaded{
        [&](Counter&, Inc const&) { self->send(Arm{}); },
        [](Counter&, Rename const&) {},
        [](Counter& state, Arm const&) { state.armed.set(true); },
    };
    Store<Counter, CounterMsg> store{runtime, initCounter, reentrant};
    self = &store;
    store.send(Inc{});
    CHECK_FALSE(store.state().armed.peek());
    CHECK(probe.count(morph::reactive::detail::site::kSendInUpdate) == 1);
    store.send(Arm{});  // the guard was released
    CHECK(store.state().armed.peek());
}

// ── request() over a real LocalBackend ─────────────────────────────────────

struct StorePingAction {
    int value = 0;
};
struct StorePingFail {};

struct StorePingModel {
    int execute(StorePingAction action) { return action.value * 2; }
    int execute(StorePingFail) { throw std::runtime_error("ping failed"); }
};

BRIDGE_REGISTER_MODEL(StorePingModel, "Test_StorePingModel")
BRIDGE_REGISTER_ACTION(StorePingModel, StorePingAction, "Test_StorePingAction")
BRIDGE_REGISTER_ACTION(StorePingModel, StorePingFail, "Test_StorePingFail")

namespace {

struct Reply {
    Signal<int> value;
    Signal<int> deliveries;
    Signal<std::string> error;
};

struct Doubled {
    int value;
};
struct Failed {
    std::exception_ptr error;
};
using ReplyMsg = std::variant<Doubled, Failed>;

Reply initReply(Runtime& runtime) {
    return Reply{.value{runtime, 0}, .deliveries{runtime, 0}, .error{runtime, ""}};
}

std::string describe(std::exception_ptr const& error) {
    try {
        std::rethrow_exception(error);
    } catch (std::exception const& exception) {
        return exception.what();
    } catch (...) {
        return "unknown";
    }
}

auto const updateReply = Overloaded{
    [](Reply& state, Doubled const& msg) {
        state.value.set(msg.value);
        state.deliveries.set(state.deliveries.peek() + 1);
    },
    [](Reply& state, Failed const& msg) { state.error.set(describe(msg.error)); },
};

auto const toDoubled = [](int value) { return Doubled{value}; };
auto const toFailed = [](std::exception_ptr error) { return Failed{std::move(error)}; };

template <typename Pred>
bool pumpUntil(morph::exec::MainThreadExecutor& owner, Pred const& done) {
    for (int step = 0; step < 400 && !done(); ++step) {
        owner.runFor(std::chrono::milliseconds{5});
    }
    return done();
}

struct Wiring {
    morph::exec::ThreadPoolExecutor pool{1};
    morph::exec::MainThreadExecutor owner;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool), owner};
    morph::bridge::BridgeHandler<StorePingModel> handler{bridge, &owner};
};

}  // namespace

TEST_CASE("reactive::request: a result arrives as its Msg", "[reactive][store]") {
    Wiring wiring;
    Runtime runtime{wiring.owner};
    Store<Reply, ReplyMsg> store{runtime, initReply, updateReply};
    morph::async::CallbackScope const scope;
    morph::reactive::request(store, wiring.handler, scope, StorePingAction{21}, toDoubled, toFailed);
    REQUIRE(pumpUntil(wiring.owner, [&] { return store.state().value.peek() == 42; }));
}

TEST_CASE("reactive::request: a failure arrives as a Msg carrying the exception", "[reactive][store]") {
    Wiring wiring;
    Runtime runtime{wiring.owner};
    Store<Reply, ReplyMsg> store{runtime, initReply, updateReply};
    morph::async::CallbackScope const scope;
    morph::reactive::request(store, wiring.handler, scope, StorePingFail{}, toDoubled, toFailed);
    REQUIRE(pumpUntil(wiring.owner, [&] { return store.state().error.peek() == "ping failed"; }));
}

TEST_CASE("reactive::request: a destroyed CallbackScope gates the reply", "[reactive][store]") {
    Wiring wiring;
    Runtime runtime{wiring.owner};
    Store<Reply, ReplyMsg> store{runtime, initReply, updateReply};
    auto gone = std::make_unique<morph::async::CallbackScope>();
    morph::async::CallbackScope const live;
    morph::reactive::request(store, wiring.handler, *gone, StorePingAction{21}, toDoubled, toFailed);
    gone.reset();
    // Same model instance, so the second reply is delivered after the first would have been.
    morph::reactive::request(store, wiring.handler, live, StorePingAction{5}, toDoubled, toFailed);
    REQUIRE(pumpUntil(wiring.owner, [&] { return store.state().value.peek() == 10; }));
    CHECK(store.state().deliveries.peek() == 1);
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build/reactive --target morph_tests`
Expected: FAIL — `'morph/reactive/store.hpp' file not found`.

- [ ] **Step 3: Implement the Store**

Create `include/morph/reactive/store.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <concepts>
#include <exception>
#include <functional>
#include <type_traits>
#include <utility>
#include <variant>

#include "../attributes.hpp"
#include "../core/bridge.hpp"
#include "../core/callback_scope.hpp"
#include "../core/registry.hpp"
#include "runtime.hpp"

/// @file
/// @brief `morph::reactive::Store`: user-intent view state, changed only by exhaustive messages.
///
/// Specified in `docs/spec/reactive/store.md`.

namespace morph::reactive {

namespace detail {

/// @brief Whether @p Update handles every alternative of the variant @p Msg.
/// @tparam Update The update callable.
/// @tparam ViewState The state type.
/// @tparam Msg Anything but a `std::variant`: never exhaustive.
template <typename Update, typename ViewState, typename Msg>
struct UpdateCoversAll : std::false_type {};

/// @brief Whether @p Update handles every alternative of `std::variant<Alts...>`.
/// @tparam Update The update callable.
/// @tparam ViewState The state type.
/// @tparam Alts The message alternatives.
template <typename Update, typename ViewState, typename... Alts>
struct UpdateCoversAll<Update, ViewState, std::variant<Alts...>>
    : std::bool_constant<(std::invocable<Update&, ViewState&, Alts const&> && ...)> {};

/// @brief Marks a Store as inside its update for the guard's lifetime.
class UpdateGuard {
public:
    /// @param flag The Store's in-update flag; set now, cleared on destruction.
    explicit UpdateGuard(bool& flag) noexcept : _flag{&flag} { flag = true; }
    ~UpdateGuard() { *_flag = false; }
    UpdateGuard(UpdateGuard const&) = delete;
    UpdateGuard& operator=(UpdateGuard const&) = delete;
    UpdateGuard(UpdateGuard&&) = delete;
    UpdateGuard& operator=(UpdateGuard&&) = delete;

private:
    bool* _flag;
};

}  // namespace detail

/// @brief An update that handles every message: invocable as `(ViewState&, Alt const&)` for each
///        alternative `Alt` of the `std::variant` @p Msg.
/// @tparam Update The update callable.
/// @tparam ViewState The state type.
/// @tparam Msg The message variant.
template <typename Update, typename ViewState, typename Msg>
concept ExhaustiveUpdate = detail::UpdateCoversAll<Update, ViewState, Msg>::value;

/// @brief User-intent view state — what is selected, typed, open — changed only by messages.
///
/// `ViewState` is a struct of `Signal<T>` fields, built in place by `init` (signals cannot move).
/// Every `send()` runs the update in one batch, untracked, so N field writes are one flush.
/// The view reads `state()`; only the update writes.
/// @tparam ViewState A struct of `Signal<T>` fields.
/// @tparam Msg A `std::variant` of message types.
template <typename ViewState, typename Msg>
class Store {
public:
    /// @tparam Init Callable `(Runtime&) -> ViewState`, returning by prvalue.
    /// @tparam Update Callable satisfying `ExhaustiveUpdate<Update, ViewState, Msg>`.
    /// @param runtime The runtime. Borrowed: it must outlive the Store.
    /// @param init Builds the initial state.
    /// @param update Applies one message to the state.
    template <typename Init, typename Update>
        requires std::invocable<Init&, Runtime&> &&
                 std::same_as<std::invoke_result_t<Init&, Runtime&>, ViewState> &&
                 ExhaustiveUpdate<Update, ViewState, Msg>
    Store(Runtime& runtime MORPH_LIFETIMEBOUND, Init init, Update update)
        : _rt{&runtime},
          _state{init(runtime)},
          _update{[upd = std::move(update)](ViewState& state, Msg const& msg) mutable {
              std::visit([&](auto const& alternative) { upd(state, alternative); }, msg);
          }} {}

    ~Store() = default;
    Store(Store const&) = delete;
    Store& operator=(Store const&) = delete;
    Store(Store&&) = delete;
    Store& operator=(Store&&) = delete;

    /// @brief Applies @p msg in one batch. Refused off the owner, and inside an update.
    /// @param msg The message.
    void send(Msg msg) {
        auto& core = *_rt->core();
        if (!core.checkOwner()) {
            return;
        }
        if (_inUpdate) {
            core.report(detail::site::kSendInUpdate);
            return;
        }
        _rt->batch([&] {
            _rt->untracked([&] {
                detail::UpdateGuard const guard{_inUpdate};
                _update(_state, msg);
            });
        });
    }

    /// @brief The state, for reading. A field's `get()` tracks exactly that field.
    /// @return The state.
    [[nodiscard]] ViewState const& state() const noexcept { return _state; }

    /// @brief A callable that sends @p msg each time it is called — what a button binds to.
    /// @param msg The message, copied into the callable.
    /// @return The callable; it refers to this Store, which must outlive it.
    [[nodiscard]] std::function<void()> action(Msg msg) {
        return [this, msg = std::move(msg)] { send(msg); };
    }

private:
    Runtime* _rt;
    ViewState _state;
    std::function<void(ViewState&, Msg const&)> _update;
    bool _inUpdate = false;
};

/// @brief Executes @p action and sends its result, or its failure, to @p store as one message.
///
/// The low-level primitive `Query` and `Mutation` are built on. Callbacks land on the handler's
/// GUI executor, which must be the Store's owner; each delivery is one batch. @p scope gates both.
/// @tparam ViewState The Store's state type.
/// @tparam Msg The Store's message variant.
/// @tparam Model The handler's model type.
/// @tparam Sharing The handler's sharing policy.
/// @tparam Action The action type.
/// @tparam ToMsg Callable `(Result const&) -> alternative of Msg`.
/// @tparam ToFailMsg Callable `(std::exception_ptr) -> alternative of Msg`.
/// @param store Receives the message. Must outlive every delivery @p scope does not gate.
/// @param handler Executes the action.
/// @param scope Gate: a reply arriving after it is stopped or destroyed is dropped.
/// @param action The action.
/// @param toMsg Maps the result to a message.
/// @param toFailMsg Maps the failure to a message.
template <typename ViewState, typename Msg, typename Model, typename Sharing, typename Action, typename ToMsg,
          typename ToFailMsg>
void request(Store<ViewState, Msg>& store, bridge::BridgeHandler<Model, Sharing>& handler,
             async::CallbackScope const& scope, Action action, ToMsg toMsg, ToFailMsg toFailMsg) {
    using Result = typename model::ActionTraits<Action>::Result;
    handler.execute(std::move(action))
        .then(scope, [&store, toMsg = std::move(toMsg)](Result const& result) { store.send(Msg{toMsg(result)}); })
        .onError(scope, [&store, toFailMsg = std::move(toFailMsg)](std::exception_ptr error) {
            store.send(Msg{toFailMsg(std::move(error))});
        });
}

}  // namespace morph::reactive
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build/reactive --target morph_tests && ./build/reactive/tests/morph_tests "[store]"`
Expected: PASS. Mutation check: in `request()`, drop the `scope` argument from `.then(…)`; expected FAIL in
"a destroyed CallbackScope gates the reply" (`deliveries == 2`). Restore.

- [ ] **Step 5: Commit**

```bash
git add include/morph/reactive/store.hpp tests/test_reactive_store.cpp tests/CMakeLists.txt CMakeLists.txt
git commit -m "wip(reactive): Store with exhaustive updates, and request()

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 6: `Query<A, R>` — a derived async resource

**Files:**
- Create: `include/morph/reactive/control.hpp`
- Modify: `CMakeLists.txt` — add `include/morph/reactive/control.hpp` after `include/morph/reactive/store.hpp`
- Modify: `tests/CMakeLists.txt` — add `test_reactive_query.cpp` after `test_reactive_store.cpp`
- Test: `tests/test_reactive_query.cpp`

**Interfaces:**
- Consumes: `Runtime`, `Signal`, `Effect`, `detail::RuntimeCore::checkOwner()`;
  `async::Completion<T>::makeSettleable(IExecutor*) -> {Completion<T>, Completion<T>::Promise}` (tests);
  `async::CallbackScope::reset()` (the supersede verb); `BridgeHandler::execute`, `BridgeHandler::subscribe<R>(
  CallbackScope const&, std::function<void(R)>)`.
- Produces (Task 7 and Parts 2–4 rely on these):
  - `reactive::errorMessage(std::exception_ptr const&) -> std::string` — `what()`, `"unknown error"` for a
    non-`std::exception`, empty for null.
  - `reactive::Refetchable` — `virtual void refetch() = 0`.
  - `reactive::Query<A, R = model::ActionTraits<A>::Result>`:
    `Query(Runtime&, std::function<async::Completion<R>(A const&)> fetch, std::function<std::optional<A>()> key)`,
    `Query(Runtime&, bridge::BridgeHandler<M, S>&, key)`, `pending() -> bool`,
    `value() -> std::optional<R> const&`, `error() -> std::exception_ptr`, `refetch()`,
    `refreshOn<Pub>(bridge::BridgeHandler<M, S>&)`.

- [ ] **Step 1: Write the failing test**

Create `tests/test_reactive_query.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <memory>
#include <morph/core/completion.hpp>
#include <morph/core/executor.hpp>
#include <morph/reactive/control.hpp>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/signal.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "owner_probe_recorder.hpp"
#include "test_support.hpp"

namespace {

using morph::async::Completion;
using morph::reactive::Effect;
using morph::reactive::Query;
using morph::reactive::Runtime;
using morph::reactive::Signal;
using Owner = morph::testing::StepExecutor;
namespace site = morph::reactive::detail::site;

struct Lookup {
    int id = 0;
    bool operator==(Lookup const&) const = default;
};

// No operator==: every re-run of the key re-issues.
struct OpaqueLookup {
    int id = 0;
};

// Stands in for a server: every fetch is held until the test settles it, in any order.
class FakeServer {
public:
    explicit FakeServer(morph::exec::IExecutor& owner) : _owner{&owner} {}

    template <typename A>
    Completion<std::string> fetch(A const& action) {
        auto [completion, promise] = Completion<std::string>::makeSettleable(_owner);
        _calls.push_back(Call{.id = action.id, .promise = std::move(promise)});
        return std::move(completion);
    }

    template <typename A>
    auto via() {
        return [this](A const& action) { return fetch(action); };
    }

    void resolve(std::size_t index, std::string value) { _calls.at(index).promise.resolve(std::move(value)); }

    void reject(std::size_t index, std::string const& what) {
        _calls.at(index).promise.reject(std::make_exception_ptr(std::runtime_error{what}));
    }

    [[nodiscard]] std::size_t calls() const { return _calls.size(); }
    [[nodiscard]] int idOf(std::size_t index) const { return _calls.at(index).id; }

private:
    struct Call {
        int id;
        Completion<std::string>::Promise promise;
    };
    morph::exec::IExecutor* _owner;
    std::vector<Call> _calls;
};

using Detail = Query<Lookup, std::string>;

}  // namespace

TEST_CASE("reactive::Query: issues its key on construction and delivers the value", "[reactive][control]") {
    Owner owner;
    Runtime runtime{owner};
    FakeServer server{owner};
    Detail detail{runtime, server.via<Lookup>(), [] { return std::optional{Lookup{1}}; }};
    REQUIRE(server.calls() == 1);
    CHECK(detail.pending());
    server.resolve(0, "one");
    owner.runAll();
    CHECK_FALSE(detail.pending());
    CHECK(detail.value() == std::optional<std::string>{"one"});
    CHECK(detail.error() == nullptr);
}

TEST_CASE("reactive::Query: a key change mid-flight drops the stale reply", "[reactive][control]") {
    Owner owner;
    Runtime runtime{owner};
    FakeServer server{owner};
    Signal<int> selected{runtime, 1};
    Detail detail{runtime, server.via<Lookup>(), [&] { return std::optional{Lookup{selected.get()}}; }};
    selected.set(2);
    owner.runAll();
    REQUIRE(server.calls() == 2);
    CHECK(server.idOf(1) == 2);

    server.resolve(1, "two");
    owner.runAll();
    server.resolve(0, "one");  // A lands after B
    owner.runAll();
    CHECK(detail.value() == std::optional<std::string>{"two"});
    CHECK_FALSE(detail.pending());
}

TEST_CASE("reactive::Query: a superseded failure leaves error() clear", "[reactive][control]") {
    Owner owner;
    Runtime runtime{owner};
    FakeServer server{owner};
    Signal<int> selected{runtime, 1};
    Detail detail{runtime, server.via<Lookup>(), [&] { return std::optional{Lookup{selected.get()}}; }};
    selected.set(2);
    owner.runAll();
    server.resolve(1, "two");
    server.reject(0, "late failure");
    owner.runAll();
    CHECK(detail.error() == nullptr);
    CHECK(detail.value() == std::optional<std::string>{"two"});
}

TEST_CASE("reactive::Query: a nullopt key is idle", "[reactive][control]") {
    Owner owner;
    Runtime runtime{owner};
    FakeServer server{owner};
    Signal<std::optional<int>> selected{runtime, std::nullopt};
    Detail detail{runtime, server.via<Lookup>(), [&]() -> std::optional<Lookup> {
                      if (auto const id = selected.get()) {
                          return Lookup{*id};
                      }
                      return std::nullopt;
                  }};
    CHECK(server.calls() == 0);
    CHECK_FALSE(detail.pending());
    CHECK_FALSE(detail.value().has_value());

    selected.set(1);
    owner.runAll();
    server.resolve(0, "one");
    owner.runAll();
    CHECK(detail.value() == std::optional<std::string>{"one"});

    selected.set(std::nullopt);
    owner.runAll();
    CHECK_FALSE(detail.value().has_value());
    CHECK_FALSE(detail.pending());
    CHECK(server.calls() == 1);
    detail.refetch();  // idle stays idle
    CHECK(server.calls() == 1);
}

TEST_CASE("reactive::Query: error, then success clears it; value survives a refetch", "[reactive][control]") {
    Owner owner;
    Runtime runtime{owner};
    FakeServer server{owner};
    Detail detail{runtime, server.via<Lookup>(), [] { return std::optional{Lookup{1}}; }};
    server.reject(0, "down");
    owner.runAll();
    CHECK(morph::reactive::errorMessage(detail.error()) == "down");
    CHECK_FALSE(detail.pending());

    detail.refetch();
    CHECK(server.calls() == 2);
    CHECK(detail.pending());
    server.resolve(1, "one");
    owner.runAll();
    CHECK(detail.error() == nullptr);

    detail.refetch();
    CHECK(detail.pending());
    CHECK(detail.value() == std::optional<std::string>{"one"});
}

TEST_CASE("reactive::Query: an unchanged comparable key issues nothing", "[reactive][control]") {
    Owner owner;
    Runtime runtime{owner};
    FakeServer server{owner};
    Signal<int> unrelated{runtime, 0};
    Detail detail{runtime, server.via<Lookup>(), [&] {
                      static_cast<void>(unrelated.get());
                      return std::optional{Lookup{1}};
                  }};
    unrelated.set(1);
    owner.runAll();
    CHECK(server.calls() == 1);
}

TEST_CASE("reactive::Query: a non-comparable key re-issues on every re-run", "[reactive][control]") {
    Owner owner;
    Runtime runtime{owner};
    FakeServer server{owner};
    Signal<int> unrelated{runtime, 0};
    Query<OpaqueLookup, std::string> detail{runtime, server.via<OpaqueLookup>(), [&] {
                                                static_cast<void>(unrelated.get());
                                                return std::optional{OpaqueLookup{1}};
                                            }};
    unrelated.set(1);
    owner.runAll();
    CHECK(server.calls() == 2);
}

TEST_CASE("reactive::Query: a destroyed Query gates late replies", "[reactive][control]") {
    Owner owner;
    Runtime runtime{owner};
    FakeServer server{owner};
    auto detail = std::make_unique<Detail>(runtime, server.via<Lookup>(), [] { return std::optional{Lookup{1}}; });
    detail.reset();
    server.resolve(0, "late");
    owner.runAll();  // a delivery into the destroyed Query would be a use-after-free under ASan
    CHECK(server.calls() == 1);
}

TEST_CASE("reactive::Query: an Effect reading it re-runs on each state change", "[reactive][control]") {
    Owner owner;
    Runtime runtime{owner};
    FakeServer server{owner};
    Detail detail{runtime, server.via<Lookup>(), [] { return std::optional{Lookup{1}}; }};
    std::vector<std::string> frames;
    Effect const render{runtime, [&] {
                            frames.push_back(detail.pending() ? "loading"
                                                              : detail.value().value_or("empty"));
                        }};
    server.resolve(0, "one");
    owner.runAll();
    CHECK(frames == std::vector<std::string>{"loading", "one"});
}

// Review Focus 4.
TEST_CASE("reactive::Query: a throwing key is reported and leaves the query as it was",
          "[reactive][control][misuse]") {
    Owner owner;
    morph::testing::OwnerProbeRecorder const probe{owner.coreExecutor()};
    Runtime runtime{owner};
    FakeServer server{owner};
    Signal<int> selected{runtime, 1};
    Detail detail{runtime, server.via<Lookup>(), [&] {
                      if (selected.get() == 2) {
                          throw std::invalid_argument{"no such id"};
                      }
                      return std::optional{Lookup{selected.get()}};
                  }};
    server.resolve(0, "one");
    owner.runAll();
    selected.set(2);
    owner.runAll();
    CHECK(probe.count(site::kEffectThrew) == 1);
    CHECK(server.calls() == 1);
    CHECK(detail.value() == std::optional<std::string>{"one"});
}

TEST_CASE("reactive::Query: a delivery off the owner is reported and dropped", "[reactive][control][misuse]") {
    Owner owner;
    morph::testing::OwnerProbeRecorder const probe{owner.coreExecutor()};
    Runtime runtime{owner};
    morph::exec::MainThreadExecutor foreign;
    FakeServer server{foreign};
    Detail detail{runtime, server.via<Lookup>(), [] { return std::optional{Lookup{1}}; }};
    server.resolve(0, "one");
    std::thread{[&] { foreign.runFor(std::chrono::milliseconds{50}); }}.join();
    CHECK(probe.count(site::kOffOwner) >= 1);
    CHECK_FALSE(detail.value().has_value());
}

TEST_CASE("reactive::errorMessage: what(), a foreign throw, and null", "[reactive][control]") {
    using morph::reactive::errorMessage;
    CHECK(errorMessage(std::make_exception_ptr(std::runtime_error{"boom"})) == "boom");
    CHECK(errorMessage(std::make_exception_ptr(42)) == "unknown error");
    CHECK(errorMessage(nullptr).empty());
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build/reactive --target morph_tests`
Expected: FAIL — `'morph/reactive/control.hpp' file not found`.

- [ ] **Step 3: Implement `errorMessage`, `Refetchable` and `Query`**

Create `include/morph/reactive/control.hpp` (Task 7 appends `Mutation` and `Subscription`):

```cpp
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <concepts>
#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "../attributes.hpp"
#include "../core/bridge.hpp"
#include "../core/callback_scope.hpp"
#include "../core/completion.hpp"
#include "../core/registry.hpp"
#include "runtime.hpp"
#include "signal.hpp"

/// @file
/// @brief Declarative control: `Query`, `Mutation` and `Subscription` turn server interaction
///        into reactive state, so a controller states what it depends on and what a write
///        invalidates instead of sequencing calls by hand.
///
/// Specified in `docs/spec/reactive/control.md`.

namespace morph::reactive {

/// @brief The one place an `exception_ptr` becomes display text.
/// @param error The captured exception, or null.
/// @return `what()` for a `std::exception`, `"unknown error"` for anything else, empty for null.
[[nodiscard]] inline std::string errorMessage(std::exception_ptr const& error) {
    if (error == nullptr) {
        return {};
    }
    try {
        std::rethrow_exception(error);
    } catch (std::exception const& exception) {
        return exception.what();
    } catch (...) {
        return "unknown error";
    }
}

/// @brief Something a `Mutation` can tell to fetch again after it succeeds.
class Refetchable {
public:
    Refetchable() = default;
    virtual ~Refetchable() = default;
    Refetchable(Refetchable const&) = delete;
    Refetchable& operator=(Refetchable const&) = delete;
    Refetchable(Refetchable&&) = delete;
    Refetchable& operator=(Refetchable&&) = delete;

    /// @brief Re-issues the current request; does nothing while idle.
    virtual void refetch() = 0;
};

/// @brief A derived async resource: the result of fetching whatever its tracked key names.
///
/// An internal Effect reads `key`. When the key changes — equality-gated when `A` is
/// equality-comparable, otherwise on every re-run — the query fetches it under a fresh generation
/// of its `CallbackScope`, so a reply for a superseded key is dropped (**latest wins**) and a call
/// that carries a stop source is asked to stop. `nullopt` means idle: nothing in flight, value and
/// error cleared. `value()` is kept while a refetch is in flight.
/// @tparam A The action (request) type.
/// @tparam R The result type; defaults to the action's registered result.
template <typename A, typename R = typename model::ActionTraits<A>::Result>
class Query final : public Refetchable {
public:
    /// @brief The tracked key: which request the query currently stands for, or `nullopt` for none.
    using Key = std::function<std::optional<A>()>;
    /// @brief Issues one request.
    using Fetch = std::function<async::Completion<R>(A const&)>;

    /// @brief Builds a query over any fetcher — the test seam, and the way to query a non-bridge source.
    /// @param runtime The runtime. Borrowed: it must outlive the query.
    /// @param fetch Issues a request; its `Completion` must deliver on the runtime's owner.
    /// @param key The tracked key.
    Query(Runtime& runtime MORPH_LIFETIMEBOUND, Fetch fetch, Key key)
        : _rt{&runtime},
          _fetch{std::move(fetch)},
          _key{std::move(key)},
          _pending{runtime, false},
          _value{runtime, std::nullopt},
          _error{runtime, nullptr} {
        _effect = std::make_unique<Effect>(runtime, [this] {
            std::optional<A> next = _key();
            _rt->untracked([&] { onKey(std::move(next)); });
        });
    }

    /// @brief Builds a query that fetches through a bridge handler.
    /// @tparam M The handler's model type.
    /// @tparam S The handler's sharing policy.
    /// @param runtime The runtime. Borrowed: it must outlive the query.
    /// @param handler Executes the requests; its GUI executor must be the runtime's owner. Borrowed.
    /// @param key The tracked key.
    template <typename M, typename S>
    Query(Runtime& runtime MORPH_LIFETIMEBOUND, bridge::BridgeHandler<M, S>& handler MORPH_LIFETIMEBOUND, Key key)
        : Query(runtime, [&handler](A const& action) { return handler.execute(action); }, std::move(key)) {}

    ~Query() override = default;
    Query(Query const&) = delete;
    Query& operator=(Query const&) = delete;
    Query(Query&&) = delete;
    Query& operator=(Query&&) = delete;

    /// @brief Whether a request for the current key is in flight. Tracked.
    /// @return True from issue to delivery.
    [[nodiscard]] bool pending() const { return _pending.get(); }

    /// @brief The last successful result. Tracked.
    /// @return The result, kept while a refetch is in flight; `nullopt` before the first or while idle.
    ///         Valid until the next delivery.
    [[nodiscard]] std::optional<R> const& value() const { return _value.get(); }

    /// @brief The last failure for the current key. Tracked.
    /// @return The exception, or null; cleared by the next success.
    [[nodiscard]] std::exception_ptr error() const { return _error.get(); }

    /// @brief Re-issues the current key under a new generation; idle stays idle.
    void refetch() override {
        if (!_rt->core()->checkOwner()) {
            return;
        }
        if (_current.has_value()) {
            issue();
        }
    }

    /// @brief Re-fetches whenever the bridge publishes a @p Pub on @p handler's instance.
    ///
    /// Uses the handler's one subscription slot for `Pub`: two consumers of the same `Pub` use two
    /// handlers.
    /// @tparam Pub The published result type to listen for.
    /// @tparam M The handler's model type.
    /// @tparam S The handler's sharing policy.
    /// @param handler The handler whose instance publishes. Borrowed: it must outlive the query.
    template <typename Pub, typename M, typename S>
    void refreshOn(bridge::BridgeHandler<M, S>& handler) {
        handler.template subscribe<Pub>(_lifetime, [this](Pub const&) { refetch(); });
    }

private:
    void onKey(std::optional<A> next) {
        if constexpr (std::equality_comparable<A>) {
            if (_keyed && next == _current) {
                return;
            }
        }
        _keyed = true;
        _current = std::move(next);
        issue();
    }

    void issue() {
        _inflight.reset();
        if (!_current.has_value()) {
            _rt->batch([&] {
                _pending.set(false);
                _value.set(std::nullopt);
                _error.set(nullptr);
            });
            return;
        }
        _pending.set(true);
        async::Completion<R> completion = _fetch(*_current);
        completion
            .then(_inflight,
                  [this](R const& result) {
                      if (!_rt->core()->checkOwner()) {
                          return;
                      }
                      _rt->batch([&] {
                          _value.set(result);
                          _error.set(nullptr);
                          _pending.set(false);
                      });
                  })
            .onError(_inflight, [this](std::exception_ptr error) {
                if (!_rt->core()->checkOwner()) {
                    return;
                }
                _rt->batch([&] {
                    _error.set(std::move(error));
                    _pending.set(false);
                });
            });
    }

    Runtime* _rt;
    Fetch _fetch;
    Key _key;
    Signal<bool> _pending;
    Signal<std::optional<R>> _value;
    Signal<std::exception_ptr> _error;
    std::optional<A> _current;
    bool _keyed = false;
    std::unique_ptr<Effect> _effect;
    // Gates the subscription `refreshOn` installs.
    async::CallbackScope _lifetime;
    // Last member, so it is destroyed first: a reply still in flight finds it stopped.
    async::CallbackScope _inflight;
};

}  // namespace morph::reactive
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build/reactive --target morph_tests && ./build/reactive/tests/morph_tests "[control]"`
Expected: PASS.

Mutation check: in `Query::issue`, delete `_inflight.reset();`. Expected FAIL in "a key change mid-flight drops the
stale reply" (`value() == "one"`). Restore.

- [ ] **Step 5: Commit**

```bash
git add include/morph/reactive/control.hpp tests/test_reactive_query.cpp tests/CMakeLists.txt CMakeLists.txt
git commit -m "wip(reactive): Query, a latest-wins async resource keyed on tracked state

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 7: `Mutation<A, R>` — a command that invalidates queries

**Files:**
- Modify: `include/morph/reactive/control.hpp` — append `MutationOptions` and `Mutation` after `Query`
- Modify: `tests/CMakeLists.txt` — add `test_reactive_mutation.cpp` after `test_reactive_query.cpp`
- Test: `tests/test_reactive_mutation.cpp`

**Interfaces:**
- Consumes: `Query`, `Refetchable`, `Runtime`, `Signal`, `async::CallbackScope`, `async::Completion`.
- Produces:
  - `reactive::MutationOptions { std::vector<Refetchable*> invalidates; }` — the listed queries must be declared
    before the mutation, so they outlive it.
  - `reactive::Mutation<A, R = model::ActionTraits<A>::Result>`:
    `Mutation(Runtime&, std::function<async::Completion<R>(A)> run, MutationOptions = {})`,
    `Mutation(Runtime&, bridge::BridgeHandler<M, S>&, MutationOptions = {})`, `run(A)`, `pending() -> bool`,
    `error() -> std::exception_ptr`, `lastResult() -> std::optional<R> const&`,
    `action(std::function<std::optional<A>()> make) -> std::function<void()>`.

- [ ] **Step 1: Write the failing test**

Create `tests/test_reactive_mutation.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <morph/core/completion.hpp>
#include <morph/reactive/control.hpp>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/signal.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "test_support.hpp"

namespace {

using morph::async::Completion;
using morph::reactive::Effect;
using morph::reactive::Mutation;
using morph::reactive::MutationOptions;
using morph::reactive::Query;
using morph::reactive::Runtime;
using morph::reactive::RuntimeOptions;
using morph::reactive::Signal;
using Owner = morph::testing::StepExecutor;

struct Lookup {
    int id = 0;
    bool operator==(Lookup const&) const = default;
};

struct Save {
    int id = 0;
};

// Holds every call until the test settles it.
template <typename Result>
class Held {
public:
    explicit Held(morph::exec::IExecutor& owner) : _owner{&owner} {}

    Completion<Result> call() {
        auto [completion, promise] = Completion<Result>::makeSettleable(_owner);
        _promises.push_back(std::move(promise));
        return std::move(completion);
    }

    void resolve(std::size_t index, Result value) { _promises.at(index).resolve(std::move(value)); }

    void reject(std::size_t index, std::string const& what) {
        _promises.at(index).reject(std::make_exception_ptr(std::runtime_error{what}));
    }

    [[nodiscard]] std::size_t calls() const { return _promises.size(); }

private:
    morph::exec::IExecutor* _owner;
    std::vector<typename Completion<Result>::Promise> _promises;
};

}  // namespace

TEST_CASE("reactive::Mutation: pending() counts overlapping calls", "[reactive][control]") {
    Owner owner;
    Runtime runtime{owner};
    Held<int> server{owner};
    Mutation<Save, int> save{runtime, [&](Save) { return server.call(); }};
    CHECK_FALSE(save.pending());
    save.run(Save{1});
    save.run(Save{2});
    CHECK(save.pending());
    server.resolve(0, 10);
    owner.runAll();
    CHECK(save.pending());
    server.resolve(1, 20);
    owner.runAll();
    CHECK_FALSE(save.pending());
    CHECK(save.lastResult() == std::optional{20});
}

TEST_CASE("reactive::Mutation: success invalidates every listed query in one flush", "[reactive][control]") {
    Owner owner;
    int frames = 0;
    Runtime runtime{owner, RuntimeOptions{.afterFlush = [&] { ++frames; }}};
    Held<std::string> listServer{owner};
    Held<std::string> detailServer{owner};
    Held<int> saveServer{owner};
    Query<Lookup, std::string> list{runtime, [&](Lookup const&) { return listServer.call(); },
                                    [] { return std::optional{Lookup{0}}; }};
    Query<Lookup, std::string> detail{runtime, [&](Lookup const&) { return detailServer.call(); },
                                      [] { return std::optional{Lookup{1}}; }};
    Mutation<Save, int> save{runtime, [&](Save) { return saveServer.call(); },
                             MutationOptions{.invalidates = {&list, &detail}}};
    Effect const render{runtime, [&] {
                            static_cast<void>(list.pending());
                            static_cast<void>(detail.pending());
                            static_cast<void>(save.pending());
                        }};
    listServer.resolve(0, "list");
    detailServer.resolve(0, "detail");
    owner.runAll();

    save.run(Save{1});
    owner.runAll();
    int const before = frames;
    saveServer.resolve(0, 1);
    owner.runAll();
    CHECK(listServer.calls() == 2);
    CHECK(detailServer.calls() == 2);
    CHECK(frames == before + 1);
    CHECK(list.pending());
    CHECK(list.value() == std::optional<std::string>{"list"});
}

TEST_CASE("reactive::Mutation: a failure sets error() and invalidates nothing", "[reactive][control]") {
    Owner owner;
    Runtime runtime{owner};
    Held<std::string> listServer{owner};
    Held<int> saveServer{owner};
    Query<Lookup, std::string> list{runtime, [&](Lookup const&) { return listServer.call(); },
                                    [] { return std::optional{Lookup{0}}; }};
    Mutation<Save, int> save{runtime, [&](Save) { return saveServer.call(); }, MutationOptions{.invalidates = {&list}}};
    save.run(Save{1});
    saveServer.reject(0, "conflict");
    owner.runAll();
    CHECK(morph::reactive::errorMessage(save.error()) == "conflict");
    CHECK_FALSE(save.pending());
    CHECK(listServer.calls() == 1);

    save.run(Save{2});
    saveServer.resolve(1, 2);
    owner.runAll();
    CHECK(save.error() == nullptr);
}

TEST_CASE("reactive::Mutation: action(make) issues only what make returns", "[reactive][control]") {
    Owner owner;
    Runtime runtime{owner};
    Held<int> server{owner};
    Signal<std::string> amount{runtime, ""};
    Mutation<Save, int> save{runtime, [&](Save) { return server.call(); }};
    auto const submit = save.action([&]() -> std::optional<Save> {
        if (amount.peek().empty()) {
            return std::nullopt;
        }
        return Save{static_cast<int>(amount.peek().size())};
    });
    submit();
    CHECK(server.calls() == 0);
    amount.set("12");
    submit();
    CHECK(server.calls() == 1);
}

// Review Focus 5.
TEST_CASE("reactive::Mutation: invalidating an idle query issues nothing", "[reactive][control]") {
    Owner owner;
    Runtime runtime{owner};
    Held<std::string> listServer{owner};
    Held<int> saveServer{owner};
    Query<Lookup, std::string> idle{runtime, [&](Lookup const&) { return listServer.call(); },
                                    [] { return std::optional<Lookup>{}; }};
    Mutation<Save, int> save{runtime, [&](Save) { return saveServer.call(); }, MutationOptions{.invalidates = {&idle}}};
    save.run(Save{1});
    saveServer.resolve(0, 1);
    owner.runAll();
    CHECK(listServer.calls() == 0);
    CHECK_FALSE(idle.pending());
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build/reactive --target morph_tests`
Expected: FAIL — `no template named 'Mutation' in namespace 'morph::reactive'`.

- [ ] **Step 3: Implement `Mutation`**

In `include/morph/reactive/control.hpp`, insert before the closing `}  // namespace morph::reactive`:

```cpp
/// @brief What a `Mutation` does after it succeeds.
struct MutationOptions {
    /// @brief Queries to refetch, in the same batch as the result, after every success. Declare them
    ///        before the mutation, so they outlive it.
    std::vector<Refetchable*> invalidates;
};

/// @brief A command: issues a write and tracks it, then refetches what the write invalidates.
///
/// `pending()` counts the calls in flight. A success records `lastResult()`, clears `error()` and
/// refetches every query in `MutationOptions::invalidates` — one batch, so one flush and one
/// frame. A failure records `error()` and invalidates nothing.
/// @tparam A The action type.
/// @tparam R The result type; defaults to the action's registered result.
template <typename A, typename R = typename model::ActionTraits<A>::Result>
class Mutation final {
public:
    /// @brief Issues one call.
    using Run = std::function<async::Completion<R>(A)>;

    /// @brief Builds a mutation over any runner — the test seam.
    /// @param runtime The runtime. Borrowed: it must outlive the mutation.
    /// @param run Issues a call; its `Completion` must deliver on the runtime's owner.
    /// @param options The queries a success invalidates.
    Mutation(Runtime& runtime MORPH_LIFETIMEBOUND, Run run, MutationOptions options = {})
        : _rt{&runtime},
          _run{std::move(run)},
          _options{std::move(options)},
          _inFlight{runtime, 0},
          _error{runtime, nullptr},
          _last{runtime, std::nullopt} {}

    /// @brief Builds a mutation that executes through a bridge handler.
    /// @tparam M The handler's model type.
    /// @tparam S The handler's sharing policy.
    /// @param runtime The runtime. Borrowed: it must outlive the mutation.
    /// @param handler Executes the calls; its GUI executor must be the runtime's owner. Borrowed.
    /// @param options The queries a success invalidates.
    template <typename M, typename S>
    Mutation(Runtime& runtime MORPH_LIFETIMEBOUND, bridge::BridgeHandler<M, S>& handler MORPH_LIFETIMEBOUND,
             MutationOptions options = {})
        : Mutation(runtime, [&handler](A action) { return handler.execute(std::move(action)); }, std::move(options)) {}

    ~Mutation() = default;
    Mutation(Mutation const&) = delete;
    Mutation& operator=(Mutation const&) = delete;
    Mutation(Mutation&&) = delete;
    Mutation& operator=(Mutation&&) = delete;

    /// @brief Issues @p action. Refused off the owner.
    /// @param action The action; built by the caller per gesture, so an idempotency key minted
    ///        while building it is fresh per click.
    void run(A action) {
        if (!_rt->core()->checkOwner()) {
            return;
        }
        _inFlight.set(_inFlight.peek() + 1);
        async::Completion<R> completion = _run(std::move(action));
        completion
            .then(_lifetime,
                  [this](R const& result) {
                      if (!_rt->core()->checkOwner()) {
                          return;
                      }
                      _rt->batch([&] {
                          _last.set(result);
                          _error.set(nullptr);
                          _inFlight.set(_inFlight.peek() - 1);
                          for (Refetchable* const query : _options.invalidates) {
                              query->refetch();
                          }
                      });
                  })
            .onError(_lifetime, [this](std::exception_ptr error) {
                if (!_rt->core()->checkOwner()) {
                    return;
                }
                _rt->batch([&] {
                    _error.set(std::move(error));
                    _inFlight.set(_inFlight.peek() - 1);
                });
            });
    }

    /// @brief A callable for a button: runs what @p make returns, or nothing when it returns `nullopt`.
    ///
    /// Bind the button's `enabled` to the same validity the `make` function checks, so a disabled
    /// button and a refused run agree.
    /// @param make Builds the action from current inputs, or `nullopt` when they are not valid.
    /// @return The callable; it refers to this mutation, which must outlive it.
    [[nodiscard]] std::function<void()> action(std::function<std::optional<A>()> make) {
        return [this, make = std::move(make)] {
            if (std::optional<A> built = make()) {
                run(std::move(*built));
            }
        };
    }

    /// @brief Whether at least one call is in flight. Tracked.
    /// @return True while the in-flight count is non-zero.
    [[nodiscard]] bool pending() const { return _inFlight.get() != 0; }

    /// @brief The most recent failure. Tracked.
    /// @return The exception, or null; cleared by the next success.
    [[nodiscard]] std::exception_ptr error() const { return _error.get(); }

    /// @brief The most recent successful result. Tracked.
    /// @return The result, or `nullopt` before the first success. Valid until the next success.
    [[nodiscard]] std::optional<R> const& lastResult() const { return _last.get(); }

private:
    Runtime* _rt;
    Run _run;
    MutationOptions _options;
    Signal<std::size_t> _inFlight;
    Signal<std::exception_ptr> _error;
    Signal<std::optional<R>> _last;
    // Last member, so it is destroyed first: a reply still in flight finds it stopped.
    async::CallbackScope _lifetime;
};
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build/reactive --target morph_tests && ./build/reactive/tests/morph_tests "[control]"`
Expected: PASS.

Mutation check: delete the `for (… _options.invalidates …)` loop. Expected FAIL in "success invalidates every
listed query in one flush" (`listServer.calls() == 1`). Restore. (Moving the loop outside the batch would *not*
fail the frame count: every write in one delivery task joins the one flush already requested, which is the
coalescing Task 1 pins.)

- [ ] **Step 5: Commit**

```bash
git add include/morph/reactive/control.hpp tests/test_reactive_mutation.cpp tests/CMakeLists.txt
git commit -m "wip(reactive): Mutation, a tracked command that invalidates queries in one batch

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 8: `Subscription<R>`, and declarative control over a real bridge

**Files:**
- Modify: `include/morph/reactive/control.hpp` — append `Subscription` after `Mutation`
- Modify: `tests/CMakeLists.txt` — add `test_reactive_control_bridge.cpp` after `test_reactive_mutation.cpp`
- Test: `tests/test_reactive_control_bridge.cpp`

**Interfaces:**
- Consumes: `Query`, `Mutation`, `MutationOptions`; `bridge::Bridge`, `bridge::BridgeHandler<M>`,
  `backend::LocalBackend`, `exec::ThreadPoolExecutor`, `exec::MainThreadExecutor::runFor`.
- Produces: `reactive::Subscription<R>(Runtime&, bridge::BridgeHandler<M, S>&)` with
  `latest() -> std::optional<R> const&` (tracked).

- [ ] **Step 1: Write the failing test**

Create `tests/test_reactive_control_bridge.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <memory>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/registry.hpp>
#include <morph/reactive/control.hpp>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/signal.hpp>
#include <optional>

struct ReactiveTimes {
    int value = 0;
    bool operator==(ReactiveTimes const&) const = default;
};
struct ReactiveReadHits {
    bool operator==(ReactiveReadHits const&) const = default;
};
struct ReactiveBump {};
struct ReactiveBumped {
    int hits = 0;
};

struct ReactiveHitModel {
    int hits = 0;
    int execute(ReactiveTimes action) { return action.value * 10; }
    int execute(ReactiveReadHits) { return hits; }
    ReactiveBumped execute(ReactiveBump) { return ReactiveBumped{++hits}; }
};

BRIDGE_REGISTER_MODEL(ReactiveHitModel, "Test_ReactiveHitModel")
BRIDGE_REGISTER_ACTION(ReactiveHitModel, ReactiveTimes, "Test_ReactiveTimes")
BRIDGE_REGISTER_ACTION(ReactiveHitModel, ReactiveReadHits, "Test_ReactiveReadHits")
BRIDGE_REGISTER_ACTION(ReactiveHitModel, ReactiveBump, "Test_ReactiveBump")

namespace {

using morph::reactive::Mutation;
using morph::reactive::MutationOptions;
using morph::reactive::Query;
using morph::reactive::Runtime;
using morph::reactive::Signal;
using morph::reactive::Subscription;

struct Wiring {
    morph::exec::ThreadPoolExecutor pool{1};
    morph::exec::MainThreadExecutor owner;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool), owner};
    morph::bridge::BridgeHandler<ReactiveHitModel> handler{bridge, &owner};
};

template <typename Pred>
bool pumpUntil(morph::exec::MainThreadExecutor& owner, Pred const& done) {
    for (int step = 0; step < 400 && !done(); ++step) {
        owner.runFor(std::chrono::milliseconds{5});
    }
    return done();
}

}  // namespace

TEST_CASE("reactive control over a bridge: a Query re-fetches when its key changes", "[reactive][control]") {
    Wiring wiring;
    Runtime runtime{wiring.owner};
    Signal<int> factor{runtime, 1};
    Query<ReactiveTimes> scaled{runtime, wiring.handler, [&] { return std::optional{ReactiveTimes{factor.get()}}; }};
    REQUIRE(pumpUntil(wiring.owner, [&] { return scaled.value() == std::optional{10}; }));
    factor.set(5);
    REQUIRE(pumpUntil(wiring.owner, [&] { return scaled.value() == std::optional{50}; }));
}

TEST_CASE("reactive control over a bridge: a Mutation invalidates a Query", "[reactive][control]") {
    Wiring wiring;
    Runtime runtime{wiring.owner};
    Query<ReactiveReadHits> hits{runtime, wiring.handler, [] { return std::optional{ReactiveReadHits{}}; }};
    Mutation<ReactiveBump> bump{runtime, wiring.handler, MutationOptions{.invalidates = {&hits}}};
    REQUIRE(pumpUntil(wiring.owner, [&] { return hits.value() == std::optional{0}; }));
    bump.run(ReactiveBump{});
    REQUIRE(pumpUntil(wiring.owner, [&] { return hits.value() == std::optional{1}; }));
    CHECK(bump.lastResult().has_value());
}

TEST_CASE("reactive control over a bridge: refreshOn re-fetches when a result is published",
          "[reactive][control]") {
    Wiring wiring;
    Runtime runtime{wiring.owner};
    Query<ReactiveReadHits> hits{runtime, wiring.handler, [] { return std::optional{ReactiveReadHits{}}; }};
    hits.refreshOn<ReactiveBumped>(wiring.handler);
    Mutation<ReactiveBump> bump{runtime, wiring.handler};  // invalidates nothing itself
    REQUIRE(pumpUntil(wiring.owner, [&] { return hits.value() == std::optional{0}; }));
    bump.run(ReactiveBump{});
    REQUIRE(pumpUntil(wiring.owner, [&] { return hits.value() == std::optional{1}; }));
}

TEST_CASE("reactive control over a bridge: a Subscription follows publishes", "[reactive][control]") {
    Wiring wiring;
    Runtime runtime{wiring.owner};
    Subscription<ReactiveBumped> latest{runtime, wiring.handler};
    Mutation<ReactiveBump> bump{runtime, wiring.handler};
    CHECK_FALSE(latest.latest().has_value());
    bump.run(ReactiveBump{});
    bump.run(ReactiveBump{});
    REQUIRE(pumpUntil(wiring.owner, [&] { return latest.latest().has_value() && latest.latest()->hits == 2; }));
}

TEST_CASE("reactive control over a bridge: a destroyed Subscription gates later publishes", "[reactive][control]") {
    Wiring wiring;
    Runtime runtime{wiring.owner};
    auto latest = std::make_unique<Subscription<ReactiveBumped>>(runtime, wiring.handler);
    Mutation<ReactiveBump> bump{runtime, wiring.handler};
    latest.reset();
    bump.run(ReactiveBump{});
    REQUIRE(pumpUntil(wiring.owner, [&] { return bump.lastResult().has_value(); }));  // ASan observes the gate
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build/reactive --target morph_tests`
Expected: FAIL — `no template named 'Subscription' in namespace 'morph::reactive'`.

- [ ] **Step 3: Implement `Subscription`**

In `include/morph/reactive/control.hpp`, insert before the closing `}  // namespace morph::reactive`:

```cpp
/// @brief The latest `R` the bridge published on a handler's instance, as reactive state.
///
/// Uses the handler's one subscription slot for `R`. The subscription is gated by a scope the
/// Subscription owns, so a publish after it is destroyed is dropped.
/// @tparam R The published result type.
template <typename R>
class Subscription final {
public:
    /// @tparam M The handler's model type.
    /// @tparam S The handler's sharing policy.
    /// @param runtime The runtime. Borrowed: it must outlive the subscription.
    /// @param handler The handler whose instance publishes; its GUI executor must be the runtime's
    ///        owner. Borrowed: it must outlive the subscription.
    template <typename M, typename S>
    Subscription(Runtime& runtime MORPH_LIFETIMEBOUND, bridge::BridgeHandler<M, S>& handler MORPH_LIFETIMEBOUND)
        : _rt{&runtime}, _latest{runtime, std::nullopt} {
        handler.template subscribe<R>(_lifetime, [this](R value) {
            if (!_rt->core()->checkOwner()) {
                return;
            }
            _latest.set(std::move(value));
        });
    }

    ~Subscription() = default;
    Subscription(Subscription const&) = delete;
    Subscription& operator=(Subscription const&) = delete;
    Subscription(Subscription&&) = delete;
    Subscription& operator=(Subscription&&) = delete;

    /// @brief The latest published value. Tracked.
    /// @return The value, or `nullopt` before the first publish. Valid until the next publish.
    [[nodiscard]] std::optional<R> const& latest() const { return _latest.get(); }

private:
    Runtime* _rt;
    Signal<std::optional<R>> _latest;
    // Last member, so it is destroyed first: a publish still in flight finds it stopped.
    async::CallbackScope _lifetime;
};
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build/reactive --target morph_tests && ./build/reactive/tests/morph_tests "[reactive]"`
Expected: PASS — every `[reactive]` case from Tasks 1–8.

Mutation check: in `Query::refreshOn`, replace the body with nothing. Expected FAIL in "refreshOn re-fetches when a
result is published" (times out at `hits.value() == 1`). Restore.

- [ ] **Step 5: Commit**

```bash
git add include/morph/reactive/control.hpp tests/test_reactive_control_bridge.cpp tests/CMakeLists.txt
git commit -m "wip(reactive): Subscription, and control over a real LocalBackend

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 9: `Scheduler`, `ManualScheduler` and `QueryOptions::refreshEvery`

Timers are a control concern (a controller's polling is a query refreshed on a period), so the scheduler interface
lives here; `ui::Scheduler` (Part 2) is an alias, and each frontend implements it (EventLoop timers on the TUI,
`QTimer` on Qt Quick).

**Files:**
- Create: `include/morph/reactive/scheduler.hpp`, `include/morph/reactive/testing/manual_scheduler.hpp`
- Modify: `include/morph/reactive/control.hpp` — `QueryOptions`, and a last `QueryOptions` parameter on both `Query`
  constructors
- Modify: `CMakeLists.txt` — `FILE_SET HEADERS`, after `include/morph/reactive/control.hpp`:
  `include/morph/reactive/scheduler.hpp` and `include/morph/reactive/testing/manual_scheduler.hpp`
- Modify: `tests/CMakeLists.txt` — add `test_reactive_scheduler.cpp` after `test_reactive_control_bridge.cpp`
- Test: `tests/test_reactive_scheduler.cpp`

**Interfaces:**
- Consumes: `Query` (Task 6), `Runtime`, `Signal`.
- Produces (the interface contract's Part 1 additions): `reactive::TimerHandle`, `reactive::Scheduler`,
  `reactive::testing::ManualScheduler{advance, pendingTimers}`, `reactive::QueryOptions{scheduler, refreshEvery}`.

- [ ] **Step 1: Write the failing test**

Create `tests/test_reactive_scheduler.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <memory>
#include <morph/core/completion.hpp>
#include <morph/reactive/control.hpp>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/scheduler.hpp>
#include <morph/reactive/testing/manual_scheduler.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "test_support.hpp"

namespace {

using morph::async::Completion;
using morph::reactive::Query;
using morph::reactive::QueryOptions;
using morph::reactive::Runtime;
using morph::reactive::TimerHandle;
using morph::reactive::testing::ManualScheduler;
using Owner = morph::testing::StepExecutor;
using namespace std::chrono_literals;

struct Lookup {
    int id = 0;
    bool operator==(Lookup const&) const = default;
};

}  // namespace

TEST_CASE("reactive::ManualScheduler: after fires once at its deadline", "[reactive][scheduler]") {
    ManualScheduler scheduler;
    int fired = 0;
    TimerHandle const handle = scheduler.after(100ms, [&] { ++fired; });
    scheduler.advance(99ms);
    CHECK(fired == 0);
    scheduler.advance(1ms);
    CHECK(fired == 1);
    scheduler.advance(1000ms);
    CHECK(fired == 1);
    CHECK(scheduler.pendingTimers() == 0);
}

TEST_CASE("reactive::ManualScheduler: every repeats until cancelled", "[reactive][scheduler]") {
    ManualScheduler scheduler;
    int fired = 0;
    TimerHandle handle = scheduler.every(10ms, [&] { ++fired; });
    scheduler.advance(35ms);
    CHECK(fired == 3);
    handle.cancel();
    CHECK_FALSE(handle.active());
    scheduler.advance(100ms);
    CHECK(fired == 3);
}

TEST_CASE("reactive::TimerHandle: destruction cancels; moving transfers", "[reactive][scheduler]") {
    ManualScheduler scheduler;
    int fired = 0;
    {
        TimerHandle const transient = scheduler.every(10ms, [&] { ++fired; });
    }
    scheduler.advance(50ms);
    CHECK(fired == 0);

    TimerHandle outer;
    {
        TimerHandle inner = scheduler.every(10ms, [&] { ++fired; });
        outer = std::move(inner);
    }
    scheduler.advance(10ms);
    CHECK(fired == 1);
}

TEST_CASE("reactive::ManualScheduler: a callback may cancel itself", "[reactive][scheduler]") {
    ManualScheduler scheduler;
    int fired = 0;
    TimerHandle handle;
    handle = scheduler.every(10ms, [&] {
        ++fired;
        handle.cancel();
    });
    scheduler.advance(100ms);
    CHECK(fired == 1);
}

TEST_CASE("reactive::ManualScheduler: a handle outliving its scheduler is safe", "[reactive][scheduler]") {
    TimerHandle handle;
    {
        ManualScheduler scheduler;
        handle = scheduler.every(10ms, [] {});
    }
    handle.cancel();  // must not touch the destroyed scheduler (ASan observes)
    CHECK_FALSE(handle.active());
}

TEST_CASE("reactive::ManualScheduler: every refuses a non-positive period", "[reactive][scheduler]") {
    ManualScheduler scheduler;
    CHECK_THROWS_AS(static_cast<void>(scheduler.every(0ms, [] {})), std::invalid_argument);
}

TEST_CASE("reactive::Query: refreshEvery re-fetches on the scheduler's period", "[reactive][scheduler][control]") {
    Owner owner;
    Runtime runtime{owner};
    ManualScheduler scheduler;
    std::vector<Completion<std::string>::Promise> promises;
    auto fetch = [&](Lookup const&) {
        auto [completion, promise] = Completion<std::string>::makeSettleable(&owner);
        promises.push_back(std::move(promise));
        return std::move(completion);
    };
    auto query = std::make_unique<Query<Lookup, std::string>>(runtime, fetch, [] { return std::optional{Lookup{1}}; },
                                                              QueryOptions{.scheduler = &scheduler,
                                                                           .refreshEvery = 1000ms});
    CHECK(promises.size() == 1);
    scheduler.advance(1000ms);
    CHECK(promises.size() == 2);
    scheduler.advance(2000ms);
    CHECK(promises.size() == 4);
    query.reset();
    CHECK(scheduler.pendingTimers() == 0);
}

TEST_CASE("reactive::Query: refreshEvery without a scheduler is refused", "[reactive][scheduler][control]") {
    Owner owner;
    Runtime runtime{owner};
    auto fetch = [&](Lookup const&) { return Completion<std::string>{}; };
    CHECK_THROWS_AS((Query<Lookup, std::string>{runtime, fetch, [] { return std::optional{Lookup{1}}; },
                                                QueryOptions{.refreshEvery = 1000ms}}),
                    std::invalid_argument);
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build/reactive --target morph_tests`
Expected: FAIL — `'morph/reactive/scheduler.hpp' file not found`.

- [ ] **Step 3: Implement the scheduler interface**

Create `include/morph/reactive/scheduler.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <chrono>
#include <functional>
#include <utility>

/// @file
/// @brief `morph::reactive::Scheduler`: timers a frontend runs on the runtime's owner executor.
///
/// Specified in `docs/spec/reactive/control.md`, "Timed refresh".

namespace morph::reactive {

/// @brief Owns one scheduled timer and cancels it when destroyed. Move-only.
class TimerHandle {
public:
    /// @brief A handle that owns no timer.
    TimerHandle() = default;

    /// @param cancel Cancels the timer; called at most once, and must not throw.
    explicit TimerHandle(std::function<void()> cancel) : _cancel{std::move(cancel)} {}

    ~TimerHandle() { cancel(); }
    TimerHandle(TimerHandle const&) = delete;
    TimerHandle& operator=(TimerHandle const&) = delete;

    /// @brief Takes over @p other's timer; @p other then owns none.
    /// @param other The handle to move from.
    TimerHandle(TimerHandle&& other) noexcept : _cancel{std::exchange(other._cancel, {})} {}

    /// @brief Cancels this handle's timer, then takes over @p other's.
    /// @param other The handle to move from.
    /// @return `*this`.
    TimerHandle& operator=(TimerHandle&& other) noexcept {
        if (this != &other) {
            cancel();
            _cancel = std::exchange(other._cancel, {});
        }
        return *this;
    }

    /// @brief Cancels the timer, if this handle still owns one.
    void cancel() noexcept {
        if (auto const stop = std::exchange(_cancel, {})) {
            stop();
        }
    }

    /// @brief Whether this handle still owns a timer to cancel.
    /// @return False after `cancel()`, after a move from it, and for a default-constructed handle.
    [[nodiscard]] bool active() const noexcept { return static_cast<bool>(_cancel); }

private:
    std::function<void()> _cancel;
};

/// @brief Runs callbacks after a delay or on a period, on the runtime's owner executor.
///
/// Every frontend provides one (spec 1 §5b); a test uses `testing::ManualScheduler`.
class Scheduler {
public:
    Scheduler() = default;
    virtual ~Scheduler() = default;
    Scheduler(Scheduler const&) = delete;
    Scheduler& operator=(Scheduler const&) = delete;
    Scheduler(Scheduler&&) = delete;
    Scheduler& operator=(Scheduler&&) = delete;

    /// @brief Runs @p fn once, @p delay from now.
    /// @param delay How long to wait.
    /// @param fn The callback.
    /// @return The handle; destroying it before the deadline cancels the call.
    [[nodiscard]] virtual TimerHandle after(std::chrono::milliseconds delay, std::function<void()> fn) = 0;

    /// @brief Runs @p fn every @p period, starting one period from now.
    /// @param period The interval; must be positive.
    /// @param fn The callback.
    /// @return The handle; destroying it stops the repetition.
    /// @throws std::invalid_argument when @p period is not positive.
    [[nodiscard]] virtual TimerHandle every(std::chrono::milliseconds period, std::function<void()> fn) = 0;
};

}  // namespace morph::reactive
```

Create `include/morph/reactive/testing/manual_scheduler.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

#include "../scheduler.hpp"

/// @file
/// @brief `morph::reactive::testing::ManualScheduler`: a scheduler whose time moves only when a test says so.

namespace morph::reactive::testing {

/// @brief A deterministic `Scheduler`: `advance()` moves its clock and fires what fell due, in deadline order, on
///        the calling thread.
class ManualScheduler final : public Scheduler {
public:
    ManualScheduler() = default;
    ~ManualScheduler() override = default;
    ManualScheduler(ManualScheduler const&) = delete;
    ManualScheduler& operator=(ManualScheduler const&) = delete;
    ManualScheduler(ManualScheduler&&) = delete;
    ManualScheduler& operator=(ManualScheduler&&) = delete;

    [[nodiscard]] TimerHandle after(std::chrono::milliseconds delay, std::function<void()> fn) override {
        return add(delay, std::chrono::milliseconds{0}, std::move(fn));
    }

    [[nodiscard]] TimerHandle every(std::chrono::milliseconds period, std::function<void()> fn) override {
        if (period.count() <= 0) {
            throw std::invalid_argument{"ManualScheduler::every: the period must be positive"};
        }
        return add(period, period, std::move(fn));
    }

    /// @brief Moves the clock forward by @p by, firing every timer that falls due, earliest first; a periodic
    ///        timer fires once per elapsed period.
    /// @param by How far to move.
    void advance(std::chrono::milliseconds by) {
        auto const target = _state->now + by;
        for (;;) {
            auto& timers = _state->timers;
            auto const due = std::ranges::min_element(timers, {}, [](Timer const& timer) { return timer.deadline; });
            if (due == timers.end() || due->deadline > target) {
                break;
            }
            _state->now = due->deadline;
            std::function<void()> const fire = due->fn;
            if (due->period.count() > 0) {
                due->deadline += due->period;
            } else {
                timers.erase(due);
            }
            fire();
        }
        _state->now = target;
    }

    /// @brief How many timers are scheduled.
    /// @return The count.
    [[nodiscard]] std::size_t pendingTimers() const { return _state->timers.size(); }

private:
    struct Timer {
        std::uint64_t id;
        std::chrono::milliseconds deadline;
        std::chrono::milliseconds period;
        std::function<void()> fn;
    };
    struct State {
        std::chrono::milliseconds now{0};
        std::uint64_t nextId = 0;
        std::vector<Timer> timers;
    };

    TimerHandle add(std::chrono::milliseconds delay, std::chrono::milliseconds period, std::function<void()> fn) {
        auto const timerId = ++_state->nextId;
        _state->timers.push_back(Timer{.id = timerId, .deadline = _state->now + delay, .period = period, .fn = std::move(fn)});
        return TimerHandle{[weak = std::weak_ptr<State>{_state}, timerId] {
            if (auto const state = weak.lock()) {
                std::erase_if(state->timers, [timerId](Timer const& timer) { return timer.id == timerId; });
            }
        }};
    }

    std::shared_ptr<State> _state = std::make_shared<State>();
};

}  // namespace morph::reactive::testing
```

- [ ] **Step 4: Add `QueryOptions` to `Query`**

In `include/morph/reactive/control.hpp`, add `#include <chrono>`, `#include <stdexcept>` and
`#include "scheduler.hpp"`, and before `Query`:

```cpp
/// @brief How a `Query` refreshes besides key changes.
struct QueryOptions {
    /// @brief Runs the timed refresh; required when `refreshEvery` is positive.
    Scheduler* scheduler = nullptr;
    /// @brief Re-fetch the current key this often; zero means no timed refresh.
    std::chrono::milliseconds refreshEvery{0};
};
```

Change the two `Query` constructors to take `QueryOptions options = {}` as their last parameter (the handler
constructor forwards it), and at the end of the fetcher constructor's body, after `_effect` is created:

```cpp
        if (options.refreshEvery.count() > 0) {
            if (options.scheduler == nullptr) {
                throw std::invalid_argument{"Query: refreshEvery needs a scheduler"};
            }
            _timer = options.scheduler->every(options.refreshEvery, [this] { refetch(); });
        }
```

Add the member `TimerHandle _timer;` directly after `std::unique_ptr<Effect> _effect;` (so it is destroyed before
the effect and the signals the refresh touches), and document both new constructor parameters with
`@param options Timed refresh; see QueryOptions.`

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cmake --build build/reactive --target morph_tests && ./build/reactive/tests/morph_tests "[scheduler]"`
Expected: PASS. Mutation check: drop `_timer = ` (discard the handle, so the timer is cancelled immediately).
Expected FAIL in "refreshEvery re-fetches on the scheduler's period". Restore.

- [ ] **Step 6: Commit**

```bash
git add include/morph/reactive/scheduler.hpp include/morph/reactive/testing include/morph/reactive/control.hpp \
        tests/test_reactive_scheduler.cpp tests/CMakeLists.txt CMakeLists.txt
git commit -m "wip(reactive): Scheduler, ManualScheduler and a Query's timed refresh

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 10: Specs, maps and changelog

**Files:**
- Create: `docs/spec/reactive/signals.md`, `docs/spec/reactive/store.md`, `docs/spec/reactive/control.md`
- Modify: `docs/spec/README.md` — "Start here" map
- Modify: `docs/ARCHITECTURE.md` — "Namespace map" table and "Header map"
- Modify: `CHANGELOG.md` — `## [Unreleased]` → `### Added`
- Modify: `docs/spec/pinned_facts.toml`, `tests/test_pinned_facts.cpp`

**Interfaces:**
- Consumes: the API exactly as Tasks 1–9 ship it. Where this text and the code disagree, fix the text.
- Produces: the authoritative spec Parts 2–4 read before touching `morph::reactive`.

- [ ] **Step 1: Write `docs/spec/reactive/signals.md`**

```markdown
# Signals — design

Design spec for `morph::reactive::Runtime`, `Signal<T>`, `Computed<T>`, `Effect` and `Scope`
(`include/morph/reactive/{runtime,signal,scope}.hpp`, internals in `detail/graph.hpp`): the
fine-grained reactive graph a morph UI's state, controllers and view bindings are built from.

## Contents

- [Nodes](#nodes)
- [Scheduling](#scheduling)
- [The algorithm](#the-algorithm)
- [Misuse: reported, then refused](#misuse-reported-then-refused)
- [Lifetimes](#lifetimes)
- [Relation to the bridge](#relation-to-the-bridge)
- [Design decisions](#design-decisions)

## Nodes

| Type | Behaviour |
|---|---|
| `Signal<T>` | A source. `get()` subscribes the innermost tracked run; `peek()` does not. `set(v)` skips a value equal to the current one when `T` is `std::equality_comparable`; `mutate(f)` always notifies. |
| `Computed<T>` | Lazy, cached in a `std::optional<T>` (so `T` needs no default constructor), equality-gated: an unchanged result stops propagation. A throwing computation rethrows to the reader; the next read retries it. |
| `Effect` | Runs once in its constructor, then in a flush after any source it read changed. |
| `Scope` | Owns nodes and adopted objects; destroys them newest first. |

Every node is non-copyable and non-movable: other nodes hold raw pointers to it. Every node takes
the `Runtime&`; there is no global and no `thread_local`.

## Scheduling

- A write outside `Runtime::batch` is a batch of one; batches nest.
- When the outermost batch ends with effects queued and no flush requested, the runtime posts
  **one** flush to its owner. A `QtExecutor` does not coalesce posts; this flag does.
- No Effect runs inside `set()`. That makes a write safe from a Qt handler or a `Completion`
  callback, and lets a controller write several signals before anything observes them.
- Writes made by effects during a flush are processed by that same flush.
- The posted flush holds the core weakly and checks whether the `Runtime` is gone.
- `RuntimeOptions::afterFlush` runs after every flush that ran at least one Effect: a frontend
  schedules its redraw there.
- `Runtime::widgetEvent(f)` runs a widget callback as one batch; a flush that starts inside one is
  refused and re-posted, so a remount never destroys a widget whose native handler is on the stack.

## The algorithm

Push-dirty, pull-value, three colours (Clean < Check < Dirty), as in Reactively and Preact signals:

1. `set()` marks direct observers Dirty and their transitive observers Check. An Effect leaving
   Clean is queued.
2. The flush takes effects in queue order. For a Check node it brings each source up to date in
   read order, stopping as soon as one reports a change (which makes the node Dirty). Only a Dirty
   node recomputes. A Computed whose result is unchanged does not dirty its observers.
3. A run records what it read in a `TrackingFrame`; afterwards the node's sources are replaced by
   that set, so a source it stopped reading no longer reaches it. A run that throws keeps the union
   of old and new sources, so later changes still reach it.

Because a node is pulled only through its sources and recomputed only when a source really
changed, an Effect reading `A` and `Computed(A)` never sees one updated and the other stale.

An Effect that writes a signal it is reading does not re-run itself for that write: it is mid-run
(Dirty), so the write cannot raise it further, and it ends Clean.

## Misuse: reported, then refused

A misuse is reported through `morph::exec::detail::noteOwner(site, owner, false)` — an assertion
in a debug build, an observation when a test installs `OwnerProbeRecorder` — and then refused the
same way in every build, so behaviour does not depend on `NDEBUG`. Site names are
`detail::site::*`.

| Misuse | Site | Refusal |
|---|---|---|
| An operation off the owner (`OwnerAffinity`: inside an owner task, or on the constructing thread outside every task) | `kOffOwner` | The write or delivery is dropped; a read proceeds untracked of the violation |
| An exception escaping an Effect | `kEffectThrew` | The flush stops; what is still queued runs in a new flush |
| One Effect run more than `maxEffectRunsPerFlush` (default 100) times in a flush | `kWriteCycle` | The flush stops and its queue is dropped |
| `set()`/`mutate()` while a Computed computes | `kSetInComputed` | The write is dropped |
| A Computed reading itself | `kComputedReadsItself` | The read throws `std::logic_error` |
| `Store::send` inside an update | `kSendInUpdate` | The message is dropped |
| A `Runtime` destroyed while nodes are alive | `kRuntimeOutlived` | Nodes keep the core alive; flushes do nothing |
| A flush starting inside `widgetEvent` | `kFlushInWidgetEvent` | Re-posted |

## Lifetimes

- Destroying a node unlinks it from its sources and observers, removes it from the flush queue and
  from every open tracking frame. Destroying a queued Effect during a flush is safe.
- A `Scope` destroys what it owns newest first: bindings die before the widgets they point at.
- The `Runtime` and every node must be destroyed on the owner.

## Relation to the bridge

This layer is UI-side only. It consumes `Completion` and `BridgeHandler::subscribe` like any
caller and never enters `BridgeHandler`; nothing in the bridge knows a reactive runtime exists.

## Design decisions

| Decision | Why |
|---|---|
| Fine-grained signals, not re-render plus diff | A bound property updates exactly the widget setter that depends on it; no virtual tree, no diff, and widget identity (focus, selection) survives updates. |
| Flushes are always posted | Running effects inside `set()` re-enters view code from wherever the write happened — a Qt handler, a `Completion` callback — and makes intermediate states visible. |
| One coalescing flag in the core | The owner executor may not coalesce (`QtExecutor` does not); N writes must still be one flush. |
| Report-then-refuse through `noteOwner` | One mechanism, already observable in morph's tests, with the same refusal in release builds. |
| Nodes hold the core by `shared_ptr` | A node outliving its `Runtime` is a defect, but not a use-after-free. |
```

- [ ] **Step 2: Write `docs/spec/reactive/store.md`**

```markdown
# Store — design

Design spec for `morph::reactive::Store<ViewState, Msg>`, `ExhaustiveUpdate` and `request()`
(`include/morph/reactive/store.hpp`): user-intent view state changed only by messages.

## Shape

- `ViewState` is a struct of `Signal<T>` fields: what is selected, what is being typed, which tab
  is open. It is not called *model*: in morph a model is the domain object a
  `BridgeHandler<Model>` talks to.
- `Msg` is a `std::variant` of message types.
- `Store(Runtime&, Init, Update)`: `init(runtime)` returns the `ViewState` by prvalue — signals cannot
  move, so the Store's member is initialised from that prvalue directly. `Update` must satisfy
  `ExhaustiveUpdate<Update, ViewState, Msg>`: invocable as `(ViewState&, Alt const&)` for every
  alternative. A missing alternative is a constraint failure at the Store's construction.

## Behaviour

- `send(msg)` applies the update in one batch, untracked: N field writes, one flush.
- `state()` returns `ViewState const&`; a field's `get()` tracks exactly that field.
- `action(msg)` returns a `std::function<void()>` that sends a copy of `msg`.
- `send` off the owner is dropped (`kOffOwner`); `send` inside an update is dropped
  (`kSendInUpdate`).

## `request()`

`request(store, handler, scope, action, toMsg, toFailMsg)` executes `action` and sends
`toMsg(result)` or `toFailMsg(exception_ptr)` as one message. Errors travel per call inside the
message, never as a shared error string. `scope` gates both callbacks. It is the primitive `Query`
and `Mutation` are built on; a controller reaches for those first ([control.md](control.md)).

## Lifetime rule

The `Store`, the `Runtime` and every mounted view outlive any callback that can still fire. The
`CallbackScope` is the **last** member of the owning object, so it is destroyed first and gates
anything in flight.

## Out of scope

Elm-style `Cmd` values. Server interaction a state change implies is a `Query` keyed on that state,
so an update never needs to issue a request.
```

- [ ] **Step 3: Write `docs/spec/reactive/control.md`**

```markdown
# Declarative control — design

Design spec for `morph::reactive::Query`, `Mutation`, `MutationOptions`, `Subscription`,
`Refetchable` and `errorMessage` (`include/morph/reactive/control.hpp`): server interaction as
reactive state, so a controller states what it depends on and what a write invalidates, and the
runtime decides when to talk to the model.

## The problem

A hand-written controller sequences calls: selecting an account calls `reloadHistory`, a deposit's
`.then` calls `refresh`, every call repeats a rethrow/catch to produce an error string, and `busy`
is a counter kept by hand. Two defects follow from that shape:

- **Stale replies win.** Select A, then B; if A's reply lands after B's, the screen shows A's data
  under B.
- **Invalidation is a call graph.** What a write must refresh lives in whichever `.then` remembered
  to call `refresh()`.

## `Query<A, R>`

`R` defaults to `model::ActionTraits<A>::Result`. Constructed from a `BridgeHandler` (the model is
deduced) or from any `std::function<Completion<R>(A const&)>` fetcher — the test seam, and the way to
query a non-bridge source — plus a tracked key `std::function<std::optional<A>()>`.

- An internal Effect reads the key. A changed key — equality-gated when `A` is
  `std::equality_comparable`, otherwise every re-run — is fetched under a fresh generation of the
  query's `CallbackScope` (`reset()`). Give an action `operator== = default` to get the skip.
- **Latest wins:** a reply from a superseded generation is dropped, success and failure alike, and a
  call carrying a stop source is asked to stop.
- `nullopt` is **idle**: nothing in flight; `value()` and `error()` cleared; `refetch()` does nothing.
- Tracked reads: `pending()`; `value()` — the last result, kept while a refetch is in flight;
  `error()` — cleared by the next success.
- `refreshOn<Pub>(handler)` re-fetches whenever the bridge publishes a `Pub` on the handler's
  instance. It uses the handler's one subscription slot for `Pub`: two consumers use two handlers.
- A key function that throws is an Effect that threw (`kEffectThrew`); the query keeps its state.
- A delivery off the runtime's owner is reported (`kOffOwner`) and dropped.
- **Timed refresh:** `QueryOptions{.scheduler, .refreshEvery}` re-fetches the current key every period on a
  `Scheduler` (`scheduler.hpp`), whose callbacks run on the runtime's owner; idle stays idle; destroying the query
  cancels the timer. `testing::ManualScheduler` drives it deterministically.

## `Mutation<A, R>`

Constructed from a `BridgeHandler` or a `std::function<Completion<R>(A)>` runner, plus
`MutationOptions{ .invalidates = {&queryA, &queryB} }`. Declare the queries before the mutation,
so they outlive it.

- `run(action)` issues one call; `pending()` is true while any call is in flight.
- On success: `lastResult()` set, `error()` cleared, every listed query refetched — one batch.
  Invalidating an idle query issues nothing.
- On failure: `error()` set; nothing invalidated.
- `action(make)` returns a button callable: `make` returns the action, or `nullopt` when inputs are
  invalid. Bind the button's `enabled` to the same validity.

## `Subscription<R>`

`latest()` is a tracked `std::optional<R>`, fed by `handler.subscribe<R>` and gated by a scope the
Subscription owns.

## `errorMessage`

`what()` for a `std::exception`, `"unknown error"` for anything else, empty for null. Views bind
to it through a `Computed`.

## The controller

A controller is a plain user struct, not a framework base class. It owns, in this order: handlers,
a `Store` if it has user-intent state, Queries, Mutations, Subscriptions, and `Computed`
projections (row formatting, masking, aggregates). Its `CallbackScope`s, inside the nodes above,
are destroyed first. It includes nothing from `morph::ui`, `morph::tui` or a toolkit, so it is
testable with an executor and a local bridge and no backend mounted, and the same controller drives
every frontend.

## Design decisions

| Decision | Why |
|---|---|
| Generations through `CallbackScope::reset()` | It is morph's supersede verb: it gates the stale reply and asks a stoppable call to stop, with no counter to keep. |
| Keep `value()` while refetching | A list does not blank between reloads; `pending()` says a reload is happening. |
| Invalidate in the success batch | The result, the cleared error, the pending count and the refetches are one flush and one frame. |
| A fetcher constructor beside the handler one | Any ordering of replies can be forced in a test with `Completion::makeSettleable`. |
| No paging or optimistic-write policy | Not needed by the screens that prove this layer; one that two examples need becomes an option rather than being hand-rolled twice. |
```

- [ ] **Step 4: Update the maps and the changelog**

In `docs/spec/README.md`, under "Start here — specs by the question they answer", add after the
"**Schema-driven UI**" group:

```markdown
**Reactive state and control**
[`reactive/signals.md`](reactive/signals.md) ·
[`reactive/store.md`](reactive/store.md) ·
[`reactive/control.md`](reactive/control.md)
```

In `docs/ARCHITECTURE.md`, "Namespace map" table, add after the `morph::forms` row:

```markdown
| `morph::reactive` | Signal graph, view state and declarative control | `Runtime`, `RuntimeOptions`, `Signal<T>`, `Computed<T>`, `Effect`, `Scope`, `Store<ViewState, Msg>`, `ExhaustiveUpdate`, `request()`, `Query<A, R>`, `Mutation<A, R>`, `MutationOptions`, `Subscription<R>`, `Refetchable`, `errorMessage` |
```

and in "Header map", after the last `####` sub-section of "Library headers (`include/morph/`)", add:

```markdown
#### `reactive/` — signal graph, view state, declarative control

| Header | Responsibility |
|---|---|
| `reactive/runtime.hpp` | `Runtime`, `RuntimeOptions` — owner, batching, one posted flush |
| `reactive/signal.hpp` | `Signal<T>`, `Computed<T>`, `Effect` |
| `reactive/scope.hpp` | `Scope` — owns nodes, destroys them newest first |
| `reactive/store.hpp` | `Store<ViewState, Msg>`, `ExhaustiveUpdate`, `request()` |
| `reactive/control.hpp` | `Query`, `QueryOptions`, `Mutation`, `MutationOptions`, `Subscription`, `Refetchable`, `errorMessage` |
| `reactive/scheduler.hpp` | `Scheduler`, `TimerHandle`; `reactive/testing/manual_scheduler.hpp`: `ManualScheduler` |
| `reactive/detail/graph.hpp` | `RuntimeCore`, `Node`, `TrackingFrame`, colours, misuse site names (detail) |
```

In `CHANGELOG.md`, under `## [Unreleased]` → `### Added`, add as the first entry:

```markdown
- **`morph::reactive`: a signal graph, view state and declarative control.** Header-only, in the
  base `morph` target.
  - `Signal<T>`, `Computed<T>`, `Effect` and `Scope` over a `Runtime` bound to an owner executor:
    glitch-free, batched, one posted flush per change, misuse reported through the owner probe and
    then refused.
  - `Store<ViewState, Msg>` with a compile-time exhaustive update, and `request()`.
  - `Query` (a latest-wins async resource keyed on tracked state), `Mutation` (a tracked command
    that refetches the queries it invalidates in one batch) and `Subscription`, over a
    `BridgeHandler` or any `Completion`-returning fetcher; `errorMessage`.
  - Specified in `docs/spec/reactive/`.
```

- [ ] **Step 5: Pin the two constants the specs state**

Append to `docs/spec/pinned_facts.toml`:

```toml

# ── morph::reactive ─────────────────────────────────────────────────────────
REACTIVE_MAX_EFFECT_RUNS_PER_FLUSH = 100   # RuntimeOptions{}.maxEffectRunsPerFlush; include/morph/reactive/runtime.hpp
REACTIVE_UNKNOWN_ERROR_TEXT = "unknown error"   # errorMessage() for a non-std exception; include/morph/reactive/control.hpp
```

Append to `tests/test_pinned_facts.cpp` (add `#include <morph/reactive/control.hpp>`,
`#include <morph/reactive/runtime.hpp>` and `#include <exception>` to its includes):

```cpp
// ── morph::reactive ──────────────────────────────────────────────────────────
//
// Run-time checks: RuntimeOptions holds a std::function, so it is not a literal type.

TEST_CASE("pinned-facts: morph::reactive defaults", "[pinned-facts]") {
    REQUIRE(morph::reactive::RuntimeOptions{}.maxEffectRunsPerFlush ==
            static_cast<std::size_t>(morph::pinned_facts::kExpected_REACTIVE_MAX_EFFECT_RUNS_PER_FLUSH));
    REQUIRE(morph::reactive::errorMessage(std::make_exception_ptr(42)) ==
            morph::pinned_facts::kExpected_REACTIVE_UNKNOWN_ERROR_TEXT);
}
```

Run: `cmake --build build/reactive --target morph_tests && ./build/reactive/tests/morph_tests "[pinned-facts]"`
(re-configure first if the generated header lacks the new keys: `cmake build/reactive`).
Expected: PASS. Change the toml value to 101, re-configure and rebuild: expected FAIL. Restore.

- [ ] **Step 6: Build the docs with warnings as errors**

```bash
cmake -S . -B build/docs -G Ninja -DMORPH_BUILD_DOCUMENTATION=ON -DMORPH_BUILD_TESTS=OFF -DMORPH_BUILD_EXAMPLES=OFF
cmake --build build/docs --target doc
```

Expected: exits 0. A warning naming a `morph/reactive` symbol is a missing `@param`/`@tparam`/`@return`: fix the
header, not the Doxygen configuration.

- [ ] **Step 7: Commit**

```bash
git add docs/spec/reactive docs/spec/README.md docs/ARCHITECTURE.md CHANGELOG.md docs/spec/pinned_facts.toml \
        tests/test_pinned_facts.cpp
git commit -m "wip(reactive): specify morph::reactive

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 11: Whole-part verification

**Files:** none new; fixes land in the files they concern.

- [ ] **Step 1: Strict build and the full suite**

```bash
cmake --build build/reactive && ctest --test-dir build/reactive --output-on-failure
```

Expected: every test passes, not only `[reactive]` — `VERIFY_INTERFACE_HEADER_SETS` compiles each new public
header standalone as part of the build.

- [ ] **Step 2: Sanitizers** (Linux; on macOS use an ASan configure of the same tree)

```bash
cmake --preset clang-asan && cmake --build --preset clang-asan
bash scripts/check_sanitizer_instrumentation.sh --binary build/clang-asan/tests/morph_tests asan
./build/clang-asan/tests/morph_tests "[reactive]"
cmake --preset clang-tsan && cmake --build --preset clang-tsan
bash scripts/check_sanitizer_instrumentation.sh --binary build/clang-tsan/tests/morph_tests tsan
./build/clang-tsan/tests/morph_tests "[reactive]"
```

Expected: clean. The destroyed-Query, destroyed-Subscription and queued-Effect-destroyed cases rely on ASan as
their observer; the instrumentation check is what makes a clean run mean something.

- [ ] **Step 3: clang-tidy over the changed lines** — the recipe in CONTRIBUTING, "Running the `clang-tidy-diff`
  gate locally", with `origin/master...HEAD` and the file count asserted non-zero.

Expected: no findings.

- [ ] **Step 4: Install/export**

```bash
bash scripts/check_install_export.sh
```

Expected: passes — its consumer translation unit includes every installed public header, so a `reactive/` header
that needs an uninstalled `detail/` header fails here.

- [ ] **Step 5: Commit any fixes**

```bash
git add -A include/morph/reactive tests docs
git commit -m "wip(reactive): fixes from the sanitizer, tidy and install gates

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

Skip the commit if there was nothing to fix, and say so in the hand-off.

- [ ] **Step 6: Squash this part into its one commit**

Follow the master plan's "Squashing a part" procedure with key `reactive` and this message:

```text
reactive: signal graph, view state and declarative control

morph::reactive, header-only in the base morph target: Signal, Computed,
Effect and Scope over a Runtime bound to an owner executor (glitch-free,
batched, one posted flush per change, misuse reported through the owner
probe and then refused); Store<ViewState, Msg> with a compile-time
exhaustive update and request(); Query, Mutation and Subscription over a
BridgeHandler or any Completion-returning fetcher. Specified in
docs/spec/reactive/.

Signed-off-by: Christian Parpart <christian@parpart.family>
```

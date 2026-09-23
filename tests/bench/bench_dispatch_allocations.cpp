// SPDX-License-Identifier: Apache-2.0

// Allocation census for one local `execute` round trip.
//
// Any scoping figure for dispatch cost -- "19 allocations, two
// CompletionStates, two strings per dispatch" -- goes stale the moment
// anything on the path changes, and the dispatch path changes often. That is
// exactly the situation where a fix gets built against a figure nobody has
// re-checked. This program exists
// so the figure can be re-checked in one command instead of being rebuilt from
// a description of how it was once obtained.
//
// **Whether it is an instrument or a gate depends on the flags.** Run bare it
// counts and prints and fails nothing, and a green run of it in that mode is
// evidence of nothing -- read the number. Given `--budget=<n>`,
// `--lookup-budget=<n>` or `--id-length-budget=<n>` it fails when the
// corresponding figure exceeds the ceiling, and
// `tests/bench/CMakeLists.txt` registers it with ctest in exactly that form
// (`bench.alloc_budget`). The ceiling lives in CMake rather than here because
// an allocation count is standard-library specific -- `std::function`'s inline
// buffer and `std::string`'s SSO threshold differ across libstdc++, libc++ and
// MSVC's STL -- so it is a per-toolchain figure with its measurements recorded
// beside it, not a constant.
//
// Method, so a later run is comparable with an earlier one:
//
//   * Global `operator new`/`delete` are replaced. Counting is off until the
//     warm-up is done, so process start-up, model registration and the first
//     50 dispatches are excluded.
//   * The workload is the smallest one there is: `Ping{int}` -> `Pong{int}`
//     through `LocalBackend` on a one-thread pool, with an inline callback
//     executor. No JSON, no socket. What is left is framework overhead.
//   * Every call is waited out before the next one starts, so the count is per
//     completed round trip rather than per queued dispatch.
//   * `--attribute` additionally prints the size of every allocation made
//     during one steady-state call. For per-*line* attribution, build with
//     `-g -no-pie -rdynamic` and add a `backtrace()` to `note()`, then resolve
//     the frames with `llvm-addr2line -a -f -i -C`; that is how a per-frame
//     breakdown is produced. `-no-pie` matters:
//     without it the recorded frames are runtime addresses and addr2line
//     resolves every one of them to `_end`.
//   * A second group of censuses counts what a *registry lookup* allocates,
//     always over ids on either side of libstdc++'s 15-character SSO
//     threshold, because the cost is entirely id-length-dependent and a
//     census over one side alone either measures zero or overstates the
//     saving. `ActionDispatcher` (`coalesce` + `requiredFieldsFor`) and
//     `journal::PayloadMigrationRegistry::find` decode nothing and execute
//     nothing, so their figure *is* the key's cost.
//     `ModelRegistryFactory::create` and
//     `BridgeHandler::executeJson` do more than look up, so theirs is a floor
//     plus the key, and what is comparable between runs is the difference
//     between a long-id and a short-id run.
//
// **The worker is gated, and that is what makes the figure comparable at all.**
// A dispatch and the handlers attached to it race: the caller returns from
// `execute()` and attaches `.then`/`.onError` while the pool thread is already
// running the strand task. Win the race and each handler is appended to a
// vector that the settle then drains; lose it and each takes
// `CompletionState`'s attach-after-ready path instead. The two cost a
// different number of allocations -- **17 and 13 per call** on this workload
// with an unpinned race -- so which one a process lands in moves the headline
// figure by four allocations for reasons that have nothing to do with the code
// under measurement. Measured, interleaved, 20 processes per configuration:
// on an idle machine every process reported ~16.95; with the machine
// oversubscribed 16 ways, 18 of 20 reported ~13.06. That is the instability
// this gate pins, reproduced and given a cause.
//
// `GatedWorkerExecutor` holds the strand task until the caller has attached
// everything, so this program always measures the attach-before-settle regime:
// the more expensive of the two, and the one a real GUI client is in, since a
// result arriving from a model strand cannot beat a `.then()` written on the
// line after `execute()` in any program that is not already racing. The figure
// is then a property of the code rather than of the machine's load, which is
// what lets `tests/bench/CMakeLists.txt` compare it against a ceiling.
//
// With the gate in place the figure is *exact*: 8.06 per call in every one of
// 84 processes across clang Release, clang Debug and gcc Debug, idle and
// oversubscribed alike. Take more than one run anyway -- a single process is a single
// sample -- but if two runs disagree here, something has changed.
//
// ── What the registry censuses measure ──────────────────────────────────────
//
// x86-64 Linux, clang 22.1.8 / libstdc++ 16.2.1, Release,
// before and after making `ActionExecuteRegistry` and
// `PayloadMigrationRegistry` transparent:
//
//                                       ids past SSO      ids inside SSO
//     PayloadMigrationRegistry::find    2.00 -> 0.00      0.00 -> 0.00
//     BridgeHandler::executeJson       24.07 -> 21.07    21.06 -> 21.06
//     ModelRegistryFactory::create      2.00 -> 2.00      1.00 -> 1.00
//
// `executeJson`'s three are the `Key`'s two `std::string`s plus the one the
// method itself built from `ModelTraits<Model>::typeId()`; after the change a
// long id and a short one cost the same, which is the property the
// `--lookup-budget` gate now holds. `create` is unchanged on purpose: it runs
// per model *instantiation*, not per request, and parking it rather than
// carrying it along for symmetry is deliberately not done.
//
// Build: `-DMORPH_BUILD_LOAD_TESTS=ON`, target `morph_bench_alloc`. See
// docs/spec/testing_strategy.md.

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <deque>
#include <exception>
#include <format>
#include <functional>
#include <iostream>
#include <memory>
#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/registry.hpp>
#include <morph/journal/journal.hpp>
#include <mutex>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <thread>

namespace {

// Per-allocation sizes recorded for the one call `--attribute` inspects. A
// fixed array rather than a vector because the recorder runs *inside*
// `operator new` and must not allocate.
constexpr std::size_t kMaxRecorded = 512;

// The counters live in a function-local static rather than at namespace scope:
// mutable globals are a finding, and a first-use-constructed aggregate of
// atomics allocates nothing, which the allocation hook below requires.
struct Census {
    std::atomic<bool> counting{false};
    std::atomic<bool> recording{false};
    std::atomic<std::size_t> allocations{0};
    std::atomic<std::size_t> bytes{0};
    std::atomic<std::size_t> recorded{0};
    std::array<std::size_t, kMaxRecorded> recordedSizes{};
};

Census& census() {
    static Census state;
    return state;
}

void note(std::size_t size) {
    Census& state = census();
    if (!state.counting.load(std::memory_order_relaxed)) {
        return;
    }
    state.allocations.fetch_add(1, std::memory_order_relaxed);
    state.bytes.fetch_add(size, std::memory_order_relaxed);
    if (state.recording.load(std::memory_order_relaxed)) {
        std::size_t const slot = state.recorded.fetch_add(1, std::memory_order_relaxed);
        if (slot < kMaxRecorded) {
            state.recordedSizes.at(slot) = size;
        }
    }
}

}  // namespace

// Replacing the global allocation functions is the whole measurement, and it
// is why this is a binary of its own rather than a case in `morph_bench`: the
// replacement is process-wide, so it would perturb any other benchmark sharing
// the binary.
// NOLINTNEXTLINE(misc-new-delete-overloads)
void* operator new(std::size_t size) {
    std::size_t const request = size == 0 ? 1 : size;
    note(request);
    // NOLINTNEXTLINE(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory)
    void* block = std::malloc(request);
    if (block == nullptr) {
        throw std::bad_alloc{};
    }
    return block;
}

// NOLINTNEXTLINE(misc-new-delete-overloads)
void* operator new[](std::size_t size) { return ::operator new(size); }

void operator delete(void* block) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory)
    std::free(block);
}
void operator delete(void* block, std::size_t /*size*/) noexcept { ::operator delete(block); }
void operator delete[](void* block) noexcept { ::operator delete(block); }
void operator delete[](void* block, std::size_t /*size*/) noexcept { ::operator delete(block); }

// External linkage so glaze's reflection can mangle the type names -- the same
// convention every other morph benchmark fixture model follows.
struct BenchAllocPing {
    int x = 0;
};
struct BenchAllocPong {
    int y = 0;
};
struct BenchAllocModel {
    BenchAllocPong execute(const BenchAllocPing& action) { return BenchAllocPong{.y = action.x * 2}; }
};

BRIDGE_REGISTER_MODEL(BenchAllocModel, "BenchAlloc_Model")
BRIDGE_REGISTER_ACTION(BenchAllocModel, BenchAllocPing, "BenchAlloc_Ping")

// Two more registered pairs, existing only to be looked up. Their ids sit
// deliberately on either side of libstdc++'s 15-character SSO threshold, so
// the lookup census below reports the two cases separately instead of
// averaging them. Observing that the registry builds two `std::string`s per
// lookup leaves the load-bearing question open: whether morph's own ids are
// long enough for those
// constructions to reach the heap. morph's real ids straddle the boundary --
// this file's own `"BenchAlloc_Model"` is 16 characters and allocates,
// `"BenchAlloc_Ping"` is 15 and does not -- so the census measures both ends
// rather than asserting either.
struct BenchAllocTinyPing {
    int x = 0;
};
struct BenchAllocTinyPong {
    int y = 0;
};
struct BenchAllocTinyModel {
    BenchAllocTinyPong execute(const BenchAllocTinyPing& action) { return BenchAllocTinyPong{.y = action.x * 2}; }
};
struct BenchAllocLongIdPing {
    int x = 0;
};
struct BenchAllocLongIdPong {
    int y = 0;
};
struct BenchAllocLongIdModel {
    BenchAllocLongIdPong execute(const BenchAllocLongIdPing& action) {
        return BenchAllocLongIdPong{.y = action.x * 2};
    }
};

BRIDGE_REGISTER_MODEL(BenchAllocTinyModel, "BA_M")
BRIDGE_REGISTER_ACTION(BenchAllocTinyModel, BenchAllocTinyPing, "BA_A")
BRIDGE_REGISTER_MODEL(BenchAllocLongIdModel, "BenchAllocLongId_ModelTypeId")
BRIDGE_REGISTER_ACTION(BenchAllocLongIdModel, BenchAllocLongIdPing, "BenchAllocLongId_ActionTypeId")

namespace {

// Runs each task on the calling thread, so the callback executor contributes
// no allocations of its own and what is counted is the dispatch path.
class InlineCallbackExecutor : public ::morph::exec::IExecutor {
public:
    void post(std::function<void()> task) override {
        if (task) {
            task();
        }
    }
};

// A one-thread worker pool whose queue can be held shut. See the file header:
// holding it across `execute()` and the two attaches is what pins the census
// to one regime instead of letting the machine's scheduling pick one.
//
// Deliberately not a `ThreadPoolExecutor` with a bolt-on gate: the hold has to
// sit between taking a task off the queue and running it, which is inside the
// worker loop.
class GatedWorkerExecutor : public ::morph::exec::IExecutor {
public:
    GatedWorkerExecutor() : _worker{[this] { loop(); }} {}

    GatedWorkerExecutor(const GatedWorkerExecutor&) = delete;
    GatedWorkerExecutor& operator=(const GatedWorkerExecutor&) = delete;
    GatedWorkerExecutor(GatedWorkerExecutor&&) = delete;
    GatedWorkerExecutor& operator=(GatedWorkerExecutor&&) = delete;

    // Drains whatever is queued before joining: the backend and bridge post
    // teardown work here, and dropping it would deadlock their destructors.
    ~GatedWorkerExecutor() override {
        {
            std::scoped_lock const lock{_mtx};
            _stopping = true;
            _held = false;
        }
        _cv.notify_all();
        _worker.join();
    }

    void post(std::function<void()> task) override {
        {
            std::scoped_lock const lock{_mtx};
            _queue.push_back(std::move(task));
        }
        _cv.notify_all();
    }

    /// @brief Stops the worker taking anything new off the queue.
    void hold() {
        std::scoped_lock const lock{_mtx};
        _held = true;
    }

    /// @brief Lets the worker run again.
    void release() {
        {
            std::scoped_lock const lock{_mtx};
            _held = false;
        }
        _cv.notify_all();
    }

private:
    void loop() {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock lock{_mtx};
                _cv.wait(lock, [this] { return (!_held && !_queue.empty()) || _stopping; });
                if (_queue.empty()) {
                    if (_stopping) {
                        return;
                    }
                    continue;
                }
                task = std::move(_queue.front());
                _queue.pop_front();
            }
            if (task) {
                task();
            }
        }
    }

    std::mutex _mtx;
    std::condition_variable _cv;
    std::deque<std::function<void()>> _queue;
    bool _held = false;
    bool _stopping = false;
    // Last member: the thread it starts runs `loop()`, which touches every
    // member above, so they must all be constructed before it exists.
    std::thread _worker;
};

constexpr int kWarmup = 50;
constexpr int kCalls = 200;
constexpr int kLookups = 200;

// Allocation census for `ActionDispatcher`'s key lookups -- the server-side
// dispatch path rather than the client-side one `run()` measures. `coalesce` and `requiredFieldsFor` are the two
// lookups that do nothing *but* look up: no JSON is decoded, no runner executes, and neither returns anything that has
// to be built. What they allocate is therefore exactly what building the stored `std::pair<std::string, std::string>`
// key costs, which is the whole of Part C's claim.
//
// Both are warmed first: `requiredFieldsFor` calls a thunk that builds the
// action's `ActionDescription` on first use and caches it for the process, so
// an unwarmed run would charge that one-off construction to the census.
//
// @param modelId  Registered model type-id to look up.
// @param actionId Registered action type-id to look up.
// @return Allocations per single lookup, averaged over `2 * kLookups` of them.
double lookupCensus(std::string_view modelId, std::string_view actionId) {
    Census& state = census();
    auto& dispatcher = ::morph::model::detail::ActionDispatcher::instance();
    for (int i = 0; i < kWarmup; ++i) {
        (void)dispatcher.coalesce(modelId, actionId);
        (void)dispatcher.requiredFieldsFor(modelId, actionId);
    }
    auto const before = state.allocations.load();
    state.counting.store(true, std::memory_order_relaxed);
    for (int i = 0; i < kLookups; ++i) {
        (void)dispatcher.coalesce(modelId, actionId);
        (void)dispatcher.requiredFieldsFor(modelId, actionId);
    }
    state.counting.store(false, std::memory_order_relaxed);
    auto const after = state.allocations.load();
    // Two lookups per iteration, and the figure is per lookup.
    return static_cast<double>(after - before) / (2.0 * kLookups);
}

// Allocation census for `ModelRegistryFactory::create`. Unlike the two above
// this is *not* a pure lookup: `create` calls the
// registered factory, which news up a holder, and then hands the holder its
// primary key. So the figure has a floor that has nothing to do with the key,
// and what a fix moves is the difference between two runs of this, not the
// figure itself.
//
// @param modelId Registered model type-id to instantiate.
// @return Allocations per single `create`, averaged over `kLookups` of them.
double registryCreateCensus(std::string_view modelId) {
    Census& state = census();
    auto& registry = ::morph::model::detail::ModelRegistryFactory::instance();
    for (int i = 0; i < kWarmup; ++i) {
        (void)registry.create(modelId);
    }
    auto const before = state.allocations.load();
    state.counting.store(true, std::memory_order_relaxed);
    for (int i = 0; i < kLookups; ++i) {
        auto holder = registry.create(modelId);
        // Destroyed here, inside the counted region. `operator delete` is
        // replaced but does not count, so only the construction shows up --
        // which is what "per create" has to mean for the figure to be
        // comparable between runs.
        (void)holder;
    }
    state.counting.store(false, std::memory_order_relaxed);
    auto const after = state.allocations.load();
    return static_cast<double>(after - before) / kLookups;
}

// Allocation census for `PayloadMigrationRegistry::find` -- the only one of
// the three registry sites that is a pure lookup: `find` hashes,
// probes and returns a pointer. Nothing else in it can allocate, so the figure
// *is* the key's cost and a fix has to take it to zero.
//
// @param actionType Registered action type-id to look up.
// @param fromSchema Registered schema fingerprint to look up.
// @return Allocations per single `find`, averaged over `kLookups` of them.
double migrationFindCensus(std::string_view actionType, std::string_view fromSchema) {
    Census& state = census();
    ::morph::journal::PayloadMigrationRegistry migrations;
    migrations.add(actionType, fromSchema, [](std::string_view payload) { return std::string{payload}; });
    for (int i = 0; i < kWarmup; ++i) {
        (void)migrations.find(actionType, fromSchema);
    }
    auto const before = state.allocations.load();
    state.counting.store(true, std::memory_order_relaxed);
    for (int i = 0; i < kLookups; ++i) {
        (void)migrations.find(actionType, fromSchema);
    }
    state.counting.store(false, std::memory_order_relaxed);
    auto const after = state.allocations.load();
    return static_cast<double>(after - before) / kLookups;
}

// Allocation census for one `BridgeHandler::executeJson` round trip --
// `ActionExecuteRegistry::execute`, measured through the only caller it has. Also not a pure lookup: `executeJson`
// decodes the body, dispatches, runs the action and encodes the result, so most of this figure is the codec. That is
// why it is measured rather than the lookup alone: how hot the site is decides whether the lookup's cost is worth
// removing, and "N allocations out of M" is the answer in that form.
//
// Gated exactly as `roundTrip` is, for the reason the file header gives.
//
// @tparam Model Registered model whose handler dispatches.
// @param pool     The gated worker the bridge runs on.
// @param handler  Handler for @p Model, already bound.
// @param actionId Registered action type-id to execute.
// @return Allocations per completed `executeJson`, averaged over `kCalls` of them.
template <typename Model>
double executeJsonCensus(GatedWorkerExecutor& pool, ::morph::bridge::BridgeHandler<Model>& handler,
                         std::string_view actionId) {
    Census& state = census();
    std::atomic<int> settled{0};
    auto once = [&] {
        int const target = settled.load(std::memory_order_relaxed) + 1;
        pool.hold();
        handler.executeJson(actionId, R"({"x":1})")
            .then([&settled](const std::string&) { settled.fetch_add(1, std::memory_order_relaxed); })
            .onError([&settled](const std::exception_ptr&) { settled.fetch_add(1, std::memory_order_relaxed); });
        pool.release();
        while (settled.load(std::memory_order_acquire) < target) {
            std::this_thread::yield();
        }
    };
    for (int i = 0; i < kWarmup; ++i) {
        once();
    }
    auto const before = state.allocations.load();
    state.counting.store(true, std::memory_order_relaxed);
    for (int i = 0; i < kCalls; ++i) {
        once();
    }
    state.counting.store(false, std::memory_order_relaxed);
    auto const after = state.allocations.load();
    return static_cast<double>(after - before) / kCalls;
}

int run(bool attribute, double budget, double lookupBudget, double idLengthBudget) {
    Census& state = census();
    GatedWorkerExecutor pool;
    InlineCallbackExecutor callbackExec;
    ::morph::bridge::Bridge bridge{std::make_unique<::morph::backend::LocalBackend>(pool)};
    ::morph::bridge::BridgeHandler<BenchAllocModel> handler{bridge, &callbackExec};
    // Only `executeJsonCensus` uses these two; they exist so that census can
    // be taken on both sides of the SSO boundary rather than on whichever
    // side `BenchAllocModel`'s own ids happen to fall.
    ::morph::bridge::BridgeHandler<BenchAllocTinyModel> tinyHandler{bridge, &callbackExec};
    ::morph::bridge::BridgeHandler<BenchAllocLongIdModel> longIdHandler{bridge, &callbackExec};

    std::atomic<int> settled{0};
    auto roundTrip = [&handler, &settled, &pool](int value) {
        int const target = settled.load(std::memory_order_relaxed) + 1;
        // Hold the worker across the dispatch and both attaches, so the strand
        // task cannot settle the completion before the handlers are on it. See
        // the file header for why the census is worth little without this.
        pool.hold();
        handler.execute(BenchAllocPing{.x = value})
            .then([&settled](const BenchAllocPong&) { settled.fetch_add(1, std::memory_order_relaxed); })
            .onError([&settled](const std::exception_ptr&) { settled.fetch_add(1, std::memory_order_relaxed); });
        pool.release();
        while (settled.load(std::memory_order_acquire) < target) {
            std::this_thread::yield();
        }
    };

    for (int i = 0; i < kWarmup; ++i) {
        roundTrip(i);
    }

    state.counting.store(true, std::memory_order_relaxed);
    for (int i = 0; i < kCalls; ++i) {
        // The recorded call is one steady-state call from the middle of the
        // run, not the first: the first still pays for whatever the warm-up
        // left cold.
        state.recording.store(attribute && i == kCalls / 2, std::memory_order_relaxed);
        roundTrip(i);
    }
    state.recording.store(false, std::memory_order_relaxed);
    state.counting.store(false, std::memory_order_relaxed);

    auto const totalAllocations = state.allocations.load();
    auto const totalBytes = state.bytes.load();
    double const perCall = static_cast<double>(totalAllocations) / kCalls;
    std::cout << std::format("local execute round-trips  : {}\n", kCalls)
              << std::format("heap allocations total     : {} ({:.2f} per call)\n", totalAllocations, perCall)
              << std::format("bytes allocated total      : {} ({:.1f} per call)\n", totalBytes,
                             static_cast<double>(totalBytes) / kCalls);

    if (attribute) {
        auto const count = state.recorded.load();
        std::cout << std::format("\nallocations in one steady-state call: {}\n", count);
        for (std::size_t i = 0; i < count && i < kMaxRecorded; ++i) {
            std::cout << std::format("  #{:2}  {} bytes\n", i, state.recordedSizes.at(i));
        }
    }

    // Run after the dispatch census, not before: `lookupCensus` toggles the
    // same global counter, and interleaving the two would fold one into the
    // other.
    double const longIdLookups = lookupCensus("BenchAllocLongId_ModelTypeId", "BenchAllocLongId_ActionTypeId");
    double const shortIdLookups = lookupCensus("BA_M", "BA_A");
    std::cout << std::format("\nActionDispatcher lookups   : {} (coalesce + requiredFieldsFor, {} times each)\n",
                             2 * kLookups, kLookups)
              << std::format("  both ids past SSO        : {:.2f} allocations per lookup\n", longIdLookups)
              << std::format("  both ids inside SSO      : {:.2f} allocations per lookup\n", shortIdLookups);

    // The three registry sites. Every one is reported for ids past the SSO
    // buffer and again for ids inside it, because the cost is entirely
    // id-length-dependent and a census over one side only would either
    // measure zero or overstate the saving. morph's real ids straddle the
    // line -- `"CreateSwimlane"` is 14 characters, one under.
    double const longMigrationFind =
        migrationFindCensus("BenchAllocLongId_ActionTypeId", "BenchAllocLongId_SchemaFingerprint");
    double const shortMigrationFind = migrationFindCensus("BA_A", "BA_S");
    double const longCreate = registryCreateCensus("BenchAllocLongId_ModelTypeId");
    double const shortCreate = registryCreateCensus("BA_M");
    double const longExecuteJson = executeJsonCensus(pool, longIdHandler, "BenchAllocLongId_ActionTypeId");
    double const shortExecuteJson = executeJsonCensus(pool, tinyHandler, "BA_A");
    std::cout << std::format("\nPayloadMigrationRegistry   : find, {} times\n", kLookups)
              << std::format("  both ids past SSO        : {:.2f} allocations per lookup\n", longMigrationFind)
              << std::format("  both ids inside SSO      : {:.2f} allocations per lookup\n", shortMigrationFind)
              << std::format("\nModelRegistryFactory       : create, {} times (holder construction included)\n",
                             kLookups)
              << std::format("  id past SSO              : {:.2f} allocations per create\n", longCreate)
              << std::format("  id inside SSO            : {:.2f} allocations per create\n", shortCreate)
              << std::format("\nBridgeHandler::executeJson : {} round trips (codec included)\n", kCalls)
              << std::format("  both ids past SSO        : {:.2f} allocations per call\n", longExecuteJson)
              << std::format("  both ids inside SSO      : {:.2f} allocations per call\n", shortExecuteJson)
              << std::format("  cost of the id length    : {:.2f} allocations per call\n",
                             longExecuteJson - shortExecuteJson);

    int status = 0;
    if (budget > 0.0) {
        if (perCall > budget) {
            std::cout << std::format("FAIL: {:.2f} allocations per call exceeds --budget={:.2f}\n", perCall, budget);
            status = 1;
        } else {
            std::cout << std::format("ok: {:.2f} allocations per call within --budget={:.2f}\n", perCall, budget);
        }
    }
    if (lookupBudget >= 0.0) {
        // Two registries under one ceiling, because both make the same claim:
        // `ActionDispatcher`'s lookups and `PayloadMigrationRegistry::find`
        // are pure lookups -- they hash, probe and return -- so the figure
        // *is* the key's cost, and zero is the only right answer for it on
        // any standard library. The third
        // registry, `ActionExecuteRegistry`, is gated separately by
        // `--id-length-budget` below, because it can only be reached through
        // a whole `executeJson` round trip.
        double const worst = std::max({longIdLookups, shortIdLookups, longMigrationFind, shortMigrationFind});
        if (worst > lookupBudget) {
            std::cout << std::format(
                "FAIL: {:.2f} allocations per registry lookup exceeds "
                "--lookup-budget={:.2f}\n",
                worst, lookupBudget);
            status = 1;
        } else {
            std::cout << std::format(
                "ok: {:.2f} allocations per registry lookup within "
                "--lookup-budget={:.2f}\n",
                worst, lookupBudget);
        }
    }
    if (idLengthBudget >= 0.0) {
        // **This ceiling is 0.5 rather than 0, and the 0.5 is measured.**
        // `ActionExecuteRegistry::execute` cannot be probed on its own -- it
        // dispatches what it finds -- so what is gated is the gap between an
        // `executeJson` over ids past the SSO buffer and one over ids inside
        // it. That gap is what building the key charged, and it should be
        // nothing.
        //
        // It reads 0.01 rather than 0.00, and that residue is a deterministic
        // one-off rather than noise: two allocations across a census's 200
        // calls, identical in all of 10 processes, and swapping the order of
        // the two censuses moves the two allocations to whichever now runs
        // first instead of to whichever has the longer ids. So it is not an
        // id-length cost, and a ceiling of exactly zero would fail on it.
        //
        // 0.5 separates that residue from the thing being guarded by a wide
        // margin in both directions: the residue is 0.01, and reverting
        // either half of the transparent key on this path takes the gap to
        // 3.00 per call (two `std::string`s for the `Key`, one for
        // `executeJson`'s own copy of `ModelTraits<Model>::typeId()`).
        double const idLengthCost = longExecuteJson - shortExecuteJson;
        if (idLengthCost > idLengthBudget) {
            std::cout << std::format(
                "FAIL: {:.2f} allocations per executeJson charged to the id's "
                "length exceeds --id-length-budget={:.2f}\n",
                idLengthCost, idLengthBudget);
            status = 1;
        } else {
            std::cout << std::format(
                "ok: {:.2f} allocations per executeJson charged to the id's "
                "length within --id-length-budget={:.2f}\n",
                idLengthCost, idLengthBudget);
        }
    }
    return status;
}

}  // namespace

int main(int argc, char** argv) {
    // Everything is inside the handler, argument parsing included: `main` must
    // not let an exception escape, and both `std::stod` and `std::format` can
    // throw. A benchmark that aborts on a typo'd flag would be a poor
    // instrument.
    try {
        bool attribute = false;
        double budget = 0.0;
        // Negative means "not requested": zero is a meaningful ceiling for the
        // lookup census (the registry should reach the heap not at all), so it
        // cannot double as the off switch the way it can for --budget.
        double lookupBudget = -1.0;
        // Same convention as --lookup-budget: negative means "not requested",
        // because zero is a meaningful ceiling here too.
        double idLengthBudget = -1.0;
        std::span<char*> const args{argv, static_cast<std::size_t>(argc)};
        for (std::size_t i = 1; i < args.size(); ++i) {
            std::string_view const arg{args[i]};
            if (arg == "--attribute") {
                attribute = true;
            } else if (arg.starts_with("--budget=")) {
                budget = std::stod(std::string{arg.substr(std::string_view{"--budget="}.size())});
            } else if (arg.starts_with("--lookup-budget=")) {
                lookupBudget = std::stod(std::string{arg.substr(std::string_view{"--lookup-budget="}.size())});
            } else if (arg.starts_with("--id-length-budget=")) {
                idLengthBudget = std::stod(std::string{arg.substr(std::string_view{"--id-length-budget="}.size())});
            } else {
                std::cout << "usage: morph_bench_alloc [--attribute] [--budget=<allocations per call>] "
                             "[--lookup-budget=<allocations per registry lookup>] "
                             "[--id-length-budget=<allocations per executeJson charged to id length>]\n";
                return 2;
            }
        }
        return run(attribute, budget, lookupBudget, idLengthBudget);
    } catch (const std::exception& exc) {
        std::cout << "FAIL: " << exc.what() << "\n";
        return 1;
    } catch (...) {
        std::cout << "FAIL: unknown exception\n";
        return 1;
    }
}

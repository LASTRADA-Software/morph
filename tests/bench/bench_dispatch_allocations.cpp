// SPDX-License-Identifier: Apache-2.0

// Allocation census for one local `execute` round trip, for morph#572.
//
// morph#572 is a performance ticket whose scope is set by a number -- "19
// allocations, two CompletionStates, two strings per dispatch", measured on
// master @ 4017228d. Three pull requests then rewrote the dispatch path
// (morph#639, morph#649, morph#654), which is exactly the situation where a
// fix gets built against a figure nobody has re-checked. This program exists
// so the figure can be re-checked in one command instead of being rebuilt from
// a description of how it was once obtained.
//
// **Whether it is an instrument or a gate depends on the flags.** Run bare it
// counts and prints and fails nothing, and a green run of it in that mode is
// evidence of nothing -- read the number. Given `--budget=<n>` or
// `--lookup-budget=<n>` it fails when the figure exceeds the ceiling, and
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
//     the frames with `llvm-addr2line -a -f -i -C`; that is how the breakdown
//     in morph#572's re-measurement comment was produced. `-no-pie` matters:
//     without it the recorded frames are runtime addresses and addr2line
//     resolves every one of them to `_end`.
//   * A second, separate census counts what an `ActionDispatcher` key lookup
//     allocates (`coalesce` + `requiredFieldsFor`, over ids on either side of
//     the SSO threshold). It decodes nothing and executes nothing, so the
//     figure is the key's cost and not the codec's -- morph#572's Part C.
//
// **The worker is gated, and that is what makes the figure comparable at all.**
// A dispatch and the handlers attached to it race: the caller returns from
// `execute()` and attaches `.then`/`.onError` while the pool thread is already
// running the strand task. Win the race and each handler is appended to a
// vector that the settle then drains; lose it and each takes
// `CompletionState`'s attach-after-ready path instead. The two cost a
// different number of allocations -- **17 and 13 per call** on this workload
// before morph#572 -- so which one a process lands in moves the headline
// figure by four allocations for reasons that have nothing to do with the code
// under measurement. Measured, interleaved, 20 processes per configuration:
// on an idle machine every process reported ~16.95; with the machine
// oversubscribed 16 ways, 18 of 20 reported ~13.06. That is morph#687's
// instability, reproduced and given a cause.
//
// `GatedWorkerExecutor` holds the strand task until the caller has attached
// everything, so this program always measures the attach-before-settle regime:
// the more expensive of the two, and the one a real GUI client is in, since a
// result arriving from a model strand cannot beat a `.then()` written on the
// line after `execute()` in any program that is not already racing. The figure
// is then a property of the code rather than of the machine's load, which is
// what lets `tests/bench/CMakeLists.txt` compare it against a ceiling.
//
// With the gate in place the figure is *exact*: 14.06 per call in every one of
// 30 processes across clang Release, clang Debug and gcc Debug, idle and
// loaded alike. Take more than one run anyway -- a single process is a single
// sample -- but if two runs disagree here, something has changed.
//
// Build: `-DMORPH_BUILD_LOAD_TESTS=ON`, target `morph_bench_alloc`. See
// docs/spec/testing_strategy.md.

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <format>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/registry.hpp>
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
// averaging them. morph#529 (folded into morph#572 as Part C) left exactly
// that question open: it observed that the registry built two `std::string`s
// per lookup, but not whether morph's own ids are long enough for those
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
    BenchAllocLongIdPong execute(const BenchAllocLongIdPing& action) { return BenchAllocLongIdPong{.y = action.x * 2}; }
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

// Allocation census for `ActionDispatcher`'s key lookups -- morph#572 Part C,
// which is about the server-side dispatch path rather than the client-side one
// `run()` measures. `coalesce` and `requiredFieldsFor` are the two lookups
// that do nothing *but* look up: no JSON is decoded, no runner executes, and
// neither returns anything that has to be built. What they allocate is
// therefore exactly what building the stored `std::pair<std::string,
// std::string>` key costs, which is the whole of Part C's claim.
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

int run(bool attribute, double budget, double lookupBudget) {
    Census& state = census();
    GatedWorkerExecutor pool;
    InlineCallbackExecutor callbackExec;
    ::morph::bridge::Bridge bridge{std::make_unique<::morph::backend::LocalBackend>(pool)};
    ::morph::bridge::BridgeHandler<BenchAllocModel> handler{bridge, &callbackExec};

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
        double const worst = std::max(longIdLookups, shortIdLookups);
        if (worst > lookupBudget) {
            std::cout << std::format("FAIL: {:.2f} allocations per dispatcher lookup exceeds "
                                     "--lookup-budget={:.2f}\n",
                                     worst, lookupBudget);
            status = 1;
        } else {
            std::cout << std::format("ok: {:.2f} allocations per dispatcher lookup within "
                                     "--lookup-budget={:.2f}\n",
                                     worst, lookupBudget);
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
        std::span<char*> const args{argv, static_cast<std::size_t>(argc)};
        for (std::size_t i = 1; i < args.size(); ++i) {
            std::string_view const arg{args[i]};
            if (arg == "--attribute") {
                attribute = true;
            } else if (arg.starts_with("--budget=")) {
                budget = std::stod(std::string{arg.substr(std::string_view{"--budget="}.size())});
            } else if (arg.starts_with("--lookup-budget=")) {
                lookupBudget = std::stod(std::string{arg.substr(std::string_view{"--lookup-budget="}.size())});
            } else {
                std::cout << "usage: morph_bench_alloc [--attribute] [--budget=<allocations per call>] "
                             "[--lookup-budget=<allocations per dispatcher lookup>]\n";
                return 2;
            }
        }
        return run(attribute, budget, lookupBudget);
    } catch (const std::exception& exc) {
        std::cout << "FAIL: " << exc.what() << "\n";
        return 1;
    } catch (...) {
        std::cout << "FAIL: unknown exception\n";
        return 1;
    }
}

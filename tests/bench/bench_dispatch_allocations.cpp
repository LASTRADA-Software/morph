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
// **It is an instrument, not a gate.** It counts and prints; it fails nothing
// unless `--budget=<n>` is passed, and it is not registered with ctest. An
// allocation count is standard-library, allocator and platform specific -- a
// ceiling that is right here would be wrong on libc++ or MSVC -- so turning it
// into a CI control needs a per-toolchain budget that nothing currently has.
// Do not cite a green run of this as evidence of anything: read the number it
// prints.
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
//     `-g -rdynamic` and add a `backtrace()` to `note()`; that is how the
//     breakdown in morph#572's re-measurement comment was produced.
//
// Build: `-DMORPH_BUILD_LOAD_TESTS=ON`, target `morph_bench_alloc`. See
// docs/spec/testing_strategy.md.

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <format>
#include <functional>
#include <iostream>
#include <memory>
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
    ::morph::exec::ThreadPoolExecutor pool{1};
    InlineCallbackExecutor callbackExec;
    ::morph::bridge::Bridge bridge{std::make_unique<::morph::backend::LocalBackend>(pool)};
    ::morph::bridge::BridgeHandler<BenchAllocModel> handler{bridge, &callbackExec};

    std::atomic<int> settled{0};
    auto roundTrip = [&handler, &settled](int value) {
        int const target = settled.load(std::memory_order_relaxed) + 1;
        handler.execute(BenchAllocPing{.x = value})
            .then([&settled](const BenchAllocPong&) { settled.fetch_add(1, std::memory_order_relaxed); })
            .onError([&settled](const std::exception_ptr&) { settled.fetch_add(1, std::memory_order_relaxed); });
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

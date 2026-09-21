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

int run(bool attribute, double budget) {
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

    if (budget > 0.0) {
        if (perCall > budget) {
            std::cout << std::format("FAIL: {:.2f} allocations per call exceeds --budget={:.2f}\n", perCall, budget);
            return 1;
        }
        std::cout << std::format("ok: {:.2f} allocations per call within --budget={:.2f}\n", perCall, budget);
    }
    return 0;
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
        std::span<char*> const args{argv, static_cast<std::size_t>(argc)};
        for (std::size_t i = 1; i < args.size(); ++i) {
            std::string_view const arg{args[i]};
            if (arg == "--attribute") {
                attribute = true;
            } else if (arg.starts_with("--budget=")) {
                budget = std::stod(std::string{arg.substr(std::string_view{"--budget="}.size())});
            } else {
                std::cout << "usage: morph_bench_alloc [--attribute] [--budget=<allocations per call>]\n";
                return 2;
            }
        }
        return run(attribute, budget);
    } catch (const std::exception& exc) {
        std::cout << "FAIL: " << exc.what() << "\n";
        return 1;
    } catch (...) {
        std::cout << "FAIL: unknown exception\n";
        return 1;
    }
}

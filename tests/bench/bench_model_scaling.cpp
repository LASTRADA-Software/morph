// SPDX-License-Identifier: Apache-2.0

// Throughput of `ModelStrands::post` over a thread pool, as threads are added.
//
// Per-model work is serial and work for different models runs in parallel, so
// with many models and a few threads the strand layer should add capacity
// close to linearly. This measures how close, for three task bodies:
// empty (the strand's own overhead is all there is), about 1 us and about 5 us
// of spinning. The bare pool runs the same bodies for comparison, so the cost
// of the strand layer can be told apart from the cost of the pool.
//
// For each thread count T, T producer threads post `total` tasks between them,
// spread round-robin over `kKeys` keys, to a pool of T workers. The rate is
// tasks completed per second from the first post to the last completion, and
// the efficiency is that rate over `T` times the one-thread rate.
//
// **This is a measurement, not a gate**: wall-clock scaling depends on the core
// count, the scheduler and what else is running, so no ceiling holds across
// machines. It fails only on a gross fault -- a lost or doubled task, counted
// exactly -- or when tasks do not complete within `kTimeout`.
//
// Build: `-DMORPH_BUILD_LOAD_TESTS=ON`, target `morph_bench_scaling`.
// See docs/spec/testing_strategy.md.

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <format>
#include <iostream>
#include <morph/core/executor.hpp>
#include <morph/core/strand.hpp>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr std::uint64_t kKeys = 1024;
constexpr std::array<std::size_t, 4> kThreadCounts{1, 2, 4, 8};
constexpr std::chrono::seconds kTimeout{120};

struct Body {
    const char* name;
    std::chrono::nanoseconds spin;
    std::size_t total;
};

constexpr std::array<Body, 3> kBodies{{
    {.name = "empty", .spin = std::chrono::nanoseconds{0}, .total = 2'000'000},
    {.name = "~1 us", .spin = std::chrono::nanoseconds{1000}, .total = 400'000},
    {.name = "~5 us", .spin = std::chrono::nanoseconds{5000}, .total = 100'000},
}};

void spinFor(std::chrono::nanoseconds spin) {
    if (spin.count() == 0) {
        return;
    }
    auto const deadline = std::chrono::steady_clock::now() + spin;
    while (std::chrono::steady_clock::now() < deadline) {
    }
}

struct Result {
    double rate = 0.0;  // tasks per second
    bool exact = true;  // every task ran exactly once, in time
};

// Posts `body.total` tasks from `threads` producers via @p post(key, task) and
// waits for them all.
template <typename PostFn>
Result measure(std::size_t threads, Body const& body, PostFn post) {
    std::atomic<std::size_t> done{0};
    std::atomic<std::size_t> ready{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> producers;
    std::size_t const perProducer = body.total / threads;
    std::size_t const expected = perProducer * threads;
    for (std::size_t p = 0; p < threads; ++p) {
        producers.emplace_back([&, p] {
            ready.fetch_add(1);
            while (!go.load()) {
                std::this_thread::yield();
            }
            for (std::size_t i = 0; i < perProducer; ++i) {
                std::uint64_t const key = (((p * perProducer) + i) % kKeys) + 1;
                post(key, [&done, spin = body.spin] {
                    spinFor(spin);
                    done.fetch_add(1, std::memory_order_relaxed);
                });
            }
        });
    }
    while (ready.load() < threads) {
        std::this_thread::yield();
    }
    auto const begin = std::chrono::steady_clock::now();
    go.store(true);
    for (auto& producer : producers) {
        producer.join();
    }
    while (done.load() < expected && std::chrono::steady_clock::now() - begin < kTimeout) {
        std::this_thread::sleep_for(std::chrono::microseconds{200});
    }
    auto const end = std::chrono::steady_clock::now();
    Result result;
    result.exact = done.load() == expected;
    // A doubled task would push the count past `expected` by the time of this
    // read at the latest; a short settle makes that visible.
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    result.exact = result.exact && done.load() == expected;
    result.rate = static_cast<double>(expected) / std::chrono::duration<double>(end - begin).count();
    return result;
}

Result measureStrands(std::size_t threads, Body const& body) {
    ::morph::exec::ThreadPoolExecutor pool{threads};
    ::morph::exec::detail::ModelStrands strands{pool};
    auto result = measure(threads, body, [&strands](std::uint64_t key, auto task) {
        strands.post(::morph::exec::detail::ModelId{key}, std::move(task));
    });
    strands.drain();
    return result;
}

Result measurePool(std::size_t threads, Body const& body) {
    ::morph::exec::ThreadPoolExecutor pool{threads};
    return measure(threads, body, [&pool](std::uint64_t /*key*/, auto task) { pool.post(std::move(task)); });
}

int run() {
    bool ok = true;
    std::cout << std::format("{} producer threads and {} workers per row; {} keys\n", "T", "T", kKeys);
    for (auto const& body : kBodies) {
        std::cout << std::format("\ntask body {} ({} tasks)\n", body.name, body.total)
                  << "  T   strands tasks/s  eff     bare pool tasks/s  eff\n";
        double strandBase = 0.0;
        double poolBase = 0.0;
        for (std::size_t const threads : kThreadCounts) {
            auto const strand = measureStrands(threads, body);
            auto const bare = measurePool(threads, body);
            if (threads == 1) {
                strandBase = strand.rate;
                poolBase = bare.rate;
            }
            double const width = static_cast<double>(threads);
            std::cout << std::format("  {}   {:>14.0f}  {:5.1f}%   {:>14.0f}  {:5.1f}%\n", threads, strand.rate,
                                     100.0 * strand.rate / (width * strandBase), bare.rate,
                                     100.0 * bare.rate / (width * poolBase));
            if (!strand.exact || !bare.exact) {
                std::cout << std::format("FAIL: T={} body {}: a task was lost, doubled or late (strands {}, pool {})\n",
                                         threads, body.name, strand.exact ? "exact" : "WRONG",
                                         bare.exact ? "exact" : "WRONG");
                ok = false;
            }
        }
    }
    return ok ? 0 : 1;
}

}  // namespace

int main() {
    try {
        return run();
    } catch (const std::exception& exc) {
        std::cout << "FAIL: " << exc.what() << "\n";
        return 1;
    } catch (...) {
        std::cout << "FAIL: unknown exception\n";
        return 1;
    }
}

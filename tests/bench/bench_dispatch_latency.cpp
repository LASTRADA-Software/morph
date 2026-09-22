// SPDX-License-Identifier: Apache-2.0

// Throughput + latency benchmark for morph::backend::RemoteServer's dispatch
// hot path, against a trivial echo model (measures framework overhead, not
// business logic). See docs/spec/testing_strategy.md.
//
// Opt-in: built only under -DMORPH_BUILD_LOAD_TESTS=ON, never part of the
// default `morph_tests` target. Writes a JSON artifact to
// BENCH_ARTIFACT_DIR/bench_dispatch_latency.json (the build directory) so CI
// can archive successive runs and diff them for regressions; also enforces a
// regression gate via CHECK on p99 latency and minimum concurrency-1
// throughput.
//
// ── What this file measures, and what it used to measure (morph#687) ────────
//
// **The serial phase used to report the test harness's polling step, not a
// round trip.** It waited on each reply with `morph::testing::WaitReply`,
// whose `await()` calls `waitUntil`, which does
// `std::this_thread::sleep_for(5ms)` between predicate checks. So an idle
// machine reported one whole sleep step per call and a busy one reported
// whatever the caller happened to observe on its first check. Measured on
// `e9dad027`, same binary, 20 processes per configuration:
//
//     idle             p50 5.0714 ms   (min 5.0562, max 5.0740)
//     16-way loaded    p50 0.0167 ms   (min 0.0166, max 0.0216)
//
// A 302x swing in the headline figure, selected by machine load -- the same
// shape of defect morph#687 recorded for `morph_bench_alloc`, and larger.
// Neither mode was the dispatch latency: the same idle processes reported
// ~176k executes/sec at concurrency 1, i.e. a round trip of about 5.7 us,
// three orders of magnitude below the 5074 us the latency phase printed.
//
// `BlockingReply` below replaces the polling waiter with a condition variable,
// so what is timed is `handle()` to reply and one thread wakeup. The
// replacement is local to this file on purpose: `waitUntil` has 464 call sites
// and changing it is not this benchmark's business, so what the rest of them
// inherit is morph#708 rather than a fix folded in here. The drain at
// the end of each throughput window is blocking for the same reason: it is
// inside the window's own elapsed time, so a 5 ms polling tail was being
// charged to the throughput figure.
//
// ── Why it reports a distribution rather than a figure ──────────────────────
//
// morph#687's other half: a cited number taken from one process is one sample.
// `morph_bench_alloc` answers that by pinning the race it was subject to, and
// an allocation count then comes out exact. A wall-clock figure has no such
// regime to pin -- contention is not a mode, it is a tax -- so this benchmark
// takes the other option morph#687 names and reports a distribution: it runs
// `MORPH_BENCH_TRIALS` trials and prints the best, median and worst of each
// percentile and each throughput point, with every trial written to the JSON
// artifact.
//
// **The gate reads the best trial, and that is the whole point.** Contention
// can only make latency worse and throughput lower, so the best of N trials is
// the least contaminated estimate of what the code costs, while a real
// regression moves every trial including the best one. Gating the median or a
// single reading is what made the concurrency-1 throughput floor unusable: on
// a 16-way oversubscribed machine, single-trial runs of the old benchmark
// spread from 311.9 to 26198.1 executes/sec against a floor of 500, so the
// gate was one busy runner away from red while being 300x too loose to catch
// anything on an idle one.
//
// Override the regression thresholds and the shape of the run with:
//   MORPH_BENCH_P99_MS_MAX=<double>          (default: 50.0)
//   MORPH_BENCH_MIN_THROUGHPUT=<double>      (default: 500.0, executes/sec at concurrency=1)
//   MORPH_BENCH_TRIALS=<int>                 (default: 5)
//   MORPH_BENCH_WINDOW_MS=<int>              (default: 200, per throughput point per trial)
//
// How the two thresholds were chosen is recorded at their definitions below.

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iostream>
#include <memory>
#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <morph/core/wire.hpp>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "test_support.hpp"

using namespace std::chrono_literals;

namespace {

double envDoubleOr(const char* name, double def) {
    const char* raw = std::getenv(name);
    if (!raw || *raw == '\0') {
        return def;
    }
    try {
        return std::stod(raw);
    } catch (const std::exception&) {
        return def;
    }
}

// Falls back to the default on a non-positive value as well as on an
// unparseable one: zero trials would make the run report nothing at all while
// still exiting green, which is the failure mode this file exists to not be.
//
// Reads through `envDoubleOr` rather than calling `std::getenv` a second time,
// so there is one place in this file that touches the environment.
int envPositiveIntOr(const char* name, int def) {
    auto const truncated = static_cast<int>(envDoubleOr(name, static_cast<double>(def)));
    return truncated > 0 ? truncated : def;
}

// Nearest-rank percentile over an already-sorted sample -- adequate for a
// benchmark's regression gate, not a statistically rigorous estimator.
double percentile(std::vector<double>& sortedMs, double p) {
    if (sortedMs.empty()) {
        return 0.0;
    }
    auto rank = static_cast<std::size_t>(p * static_cast<double>(sortedMs.size() - 1));
    return sortedMs[rank];
}

// Median of an unsorted copy -- used over trials, never over the latency
// sample, which `percentile` handles.
double medianOf(std::vector<double> values) {
    if (values.empty()) {
        return 0.0;
    }
    std::ranges::sort(values);
    return values[values.size() / 2];
}

/// @brief A `RemoteServer` reply sink that blocks on a condition variable.
///
/// `morph::testing::WaitReply` polls with a 5 ms sleep, which is about 900
/// times this benchmark's whole round trip -- see the file header. Everything
/// else about it is the same: it is passed by `std::ref` as the reply callback
/// and decodes the envelope in the callback, so what the timer sees is what a
/// caller sees.
class BlockingReply {
public:
    /// @brief Reply-callback entry point, invoked on whichever thread replies.
    /// @param msg The encoded reply envelope.
    void operator()(const std::string& msg) {
        {
            std::scoped_lock const lock{_mtx};
            try {
                _env = ::morph::wire::decode(msg);
            } catch (...) {
                // Reset rather than swallow: a decode that threw part-way may
                // have assigned some fields already, and the caller's REQUIRE
                // on `kind == "ok"` has to fail on an empty kind rather than
                // on a half-filled envelope. Rethrowing is not available --
                // this runs on a reply thread, where an escaping exception
                // terminates the process instead of failing a test.
                _env = ::morph::wire::Envelope{};
            }
            _ready = true;
        }
        _cv.notify_one();
    }

    /// @brief Blocks until the reply arrives or @p budget elapses.
    /// @param budget Longest time to wait.
    /// @return `true` if a reply arrived within the budget.
    bool await(std::chrono::milliseconds budget = 5000ms) {
        std::unique_lock lock{_mtx};
        return _cv.wait_for(lock, budget, [this] { return _ready; });
    }

    /// @brief The decoded reply envelope. Only meaningful after a `true` `await`.
    /// @return Const reference to the decoded envelope.
    [[nodiscard]] const ::morph::wire::Envelope& envelope() const { return _env; }

private:
    std::mutex _mtx;
    std::condition_variable _cv;
    bool _ready = false;
    ::morph::wire::Envelope _env;
};

/// @brief Counts completions and lets one waiter block until none are in flight.
///
/// The old drain polled at the same 5 ms granularity the latency phase did,
/// and the poll sat *inside* the window's elapsed time, so up to 5 ms of dead
/// time was divided into every throughput figure.
class InFlightGate {
public:
    /// @brief Registers one more dispatch as issued.
    void issued() {
        std::scoped_lock const lock{_mtx};
        ++_inFlight;
    }

    /// @brief Registers one dispatch as completed, waking a drain if it was the last.
    void completed() {
        {
            std::scoped_lock const lock{_mtx};
            --_inFlight;
            ++_completed;
        }
        _cv.notify_all();
    }

    /// @brief Whether fewer than @p limit dispatches are outstanding.
    /// @param limit Concurrency ceiling.
    /// @return `true` if another dispatch may be issued.
    [[nodiscard]] bool belowLimit(int limit) {
        std::scoped_lock const lock{_mtx};
        return _inFlight < limit;
    }

    /// @brief Blocks until nothing is outstanding.
    /// @param budget Longest time to wait.
    /// @return `true` if the queue drained within the budget.
    bool drain(std::chrono::milliseconds budget) {
        std::unique_lock lock{_mtx};
        return _cv.wait_for(lock, budget, [this] { return _inFlight == 0; });
    }

    /// @brief How many dispatches have completed since construction.
    /// @return The completion count.
    [[nodiscard]] std::uint64_t completedCount() {
        std::scoped_lock const lock{_mtx};
        return _completed;
    }

private:
    std::mutex _mtx;
    std::condition_variable _cv;
    int _inFlight = 0;
    std::uint64_t _completed = 0;
};

constexpr std::array<int, 5> kConcurrencies{1, 2, 4, 8, 16};

struct Trial {
    double p50Ms = 0.0;
    double p95Ms = 0.0;
    double p99Ms = 0.0;
    std::array<double, kConcurrencies.size()> throughput{};
};

}  // namespace

// Must have external linkage so Glaze's reflection can mangle the type name
// (matches the convention every other morph test fixture model follows) --
// putting these inside the anonymous namespace above fails to compile with
// "used but not defined in this translation unit, and cannot be defined in
// any other translation unit because its type does not have linkage".
struct BenchEchoAction {
    std::string s;
};
struct BenchEchoModel {
    std::string execute(const BenchEchoAction& act) { return act.s; }
};

BRIDGE_REGISTER_MODEL(BenchEchoModel, "Bench_EchoModel")
BRIDGE_REGISTER_ACTION(BenchEchoModel, BenchEchoAction, "Bench_EchoAction")

TEST_CASE("bench: RemoteServer dispatch throughput and latency", "[bench]") {
    // **The two ceilings are unchanged, and what changed is that they are now
    // applied to the best of `trials` trials rather than to one reading.**
    // That is the whole of what this pair gained: the old single-reading form
    // was simultaneously far too loose to catch a regression and tight enough
    // to fire on a busy runner. Both halves of that are measured.
    //
    // Measured on `e9dad027`, x86-64 Linux, clang 22.1.8 / libstdc++ 16.2.1,
    // 12-core machine, 20 processes per configuration (min / median / max
    // across processes), once idle and once with the machine 16-way
    // oversubscribed by busy-loop processes.
    //
    // Before, Release, the single reading the old file gated on:
    //
    //     idle     p50 5.0562 / 5.0714 / 5.0740 ms   c=1 171467 / 175928 / 178855 /sec
    //     loaded   p50 0.0166 / 0.0167 / 0.0216 ms   c=1  311.9 / 1749.9 / 18627.1 /sec
    //
    // 311.9 executes/sec is below the 500/sec floor this file has always
    // enforced, so the old gate was already firing on machine load rather
    // than on morph. After, gating the best trial of five, same sweeps:
    //
    //     Release idle     p99 0.0090 / 0.0096 / 0.0104 ms   c=1 173169 / 176282 / 181641 /sec
    //     Release loaded   p99 0.0181 / 0.0210 / 0.0259 ms   c=1  56825 /  61211 /  63479 /sec
    //     Debug   idle     p99 0.0325 / 0.0343 / 0.0360 ms   c=1  42560 /  43150 /  43996 /sec
    //     Debug   loaded   p99 0.0598 / 0.0608 / 1.8717 ms   c=1   1549 /  22684 /  22990 /sec
    //
    // The Debug rows are the ones the defaults have to hold for: the CI leg
    // that builds `MORPH_BUILD_LOAD_TESTS=ON` is `linux-all-features` on the
    // `gcc-debug`/`clang-debug` presets, and its `ctest --preset` passes no
    // label filter, so this case runs there on a Debug build.
    //
    // **So the defaults stay loose, and that is a measurement and not
    // timidity.** Even reduced to the best of five trials these are wall-clock
    // figures, and Debug-under-load still spans 28x on throughput (1549 to
    // 43996) and 57x on p99 (0.0325 ms to 1.8717 ms) for the same binary. No
    // pair of constants separates a regression from a contended runner across
    // that span, so this pair is a gross-failure detector: 50 ms is ~27x the
    // worst best-trial p99 measured and 500/sec is ~3.1x below the worst
    // best-trial throughput. A dispatch path made three times slower passes
    // both. Diff the JSON artifact's `trial_detail` against an archived run
    // for anything finer -- that is what the distribution is for, and it is
    // why this file now writes one.
    //
    // Tightening them needs the CI runner characterised rather than guessed
    // at, which is morph#707. Setting them from this 12-core box was tried
    // and rejected on the measurement above: 20000/sec turned 3 of 20
    // Debug-under-load processes red.
    const double p99MsMax = envDoubleOr("MORPH_BENCH_P99_MS_MAX", 50.0);
    const double minThroughput = envDoubleOr("MORPH_BENCH_MIN_THROUGHPUT", 500.0);
    const int trials = envPositiveIntOr("MORPH_BENCH_TRIALS", 5);
    const auto windowMs = std::chrono::milliseconds{envPositiveIntOr("MORPH_BENCH_WINDOW_MS", 200)};

    morph::exec::ThreadPoolExecutor pool{4};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);

    BlockingReply regWaiter;
    server->handle(morph::wire::encode(morph::wire::makeRegister("Bench_EchoModel")), std::ref(regWaiter));
    REQUIRE(regWaiter.await());
    REQUIRE(regWaiter.envelope().kind == "ok");
    const uint64_t modelId = regWaiter.envelope().modelId;

    morph::wire::Envelope req;
    req.kind = "execute";
    req.modelId = modelId;
    req.modelType = "Bench_EchoModel";
    req.actionType = "Bench_EchoAction";
    req.body = R"({"s":"hello"})";

    constexpr int latencySamples = 2000;
    uint64_t nextCallId = 1;
    std::vector<Trial> results;
    results.reserve(static_cast<std::size_t>(trials));

    for (int trial = 0; trial < trials; ++trial) {
        Trial out;

        // ── Phase A: serial (concurrency=1) latency distribution ────────────
        std::vector<double> latenciesMs;
        latenciesMs.reserve(latencySamples);
        for (int i = 0; i < latencySamples; ++i) {
            req.callId = nextCallId++;
            BlockingReply waiter;
            const auto start = std::chrono::steady_clock::now();
            server->handle(morph::wire::encode(req), std::ref(waiter));
            REQUIRE(waiter.await());
            const auto end = std::chrono::steady_clock::now();
            REQUIRE(waiter.envelope().kind == "ok");
            latenciesMs.push_back(std::chrono::duration<double, std::milli>(end - start).count());
        }
        std::ranges::sort(latenciesMs);
        out.p50Ms = percentile(latenciesMs, 0.50);
        out.p95Ms = percentile(latenciesMs, 0.95);
        out.p99Ms = percentile(latenciesMs, 0.99);

        // ── Phase B: throughput at increasing concurrency ────────────────────
        for (std::size_t slot = 0; slot < kConcurrencies.size(); ++slot) {
            int const concurrency = kConcurrencies.at(slot);
            InFlightGate gate;
            const auto windowStart = std::chrono::steady_clock::now();
            const auto windowEnd = windowStart + windowMs;
            while (std::chrono::steady_clock::now() < windowEnd) {
                if (!gate.belowLimit(concurrency)) {
                    std::this_thread::yield();
                    continue;
                }
                gate.issued();
                morph::wire::Envelope call = req;
                call.callId = nextCallId++;
                server->handle(morph::wire::encode(call), [&gate](const std::string&) { gate.completed(); });
            }
            REQUIRE(gate.drain(5000ms));
            const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - windowStart).count();
            out.throughput.at(slot) = static_cast<double>(gate.completedCount()) / elapsed;
        }

        results.push_back(out);
    }

    // ── Reduce over trials ───────────────────────────────────────────────────
    auto column = [&results](double Trial::* field) {
        std::vector<double> values;
        values.reserve(results.size());
        for (const Trial& row : results) {
            values.push_back(row.*field);
        }
        return values;
    };
    std::array<std::vector<double>, kConcurrencies.size()> throughputColumns;
    for (std::size_t slot = 0; slot < kConcurrencies.size(); ++slot) {
        throughputColumns.at(slot).reserve(results.size());
        for (const Trial& row : results) {
            throughputColumns.at(slot).push_back(row.throughput.at(slot));
        }
    }

    const std::vector<double> p50s = column(&Trial::p50Ms);
    const std::vector<double> p95s = column(&Trial::p95Ms);
    const std::vector<double> p99s = column(&Trial::p99Ms);

    // Best trial: lowest latency, highest throughput. See the file header for
    // why the gate reads this one rather than the median or a single run.
    const double p50Best = *std::ranges::min_element(p50s);
    const double p95Best = *std::ranges::min_element(p95s);
    const double p99Best = *std::ranges::min_element(p99s);
    const double p99Worst = *std::ranges::max_element(p99s);

    // ── Report + artifact ────────────────────────────────────────────────────
    //
    // On stdout rather than only in Catch2's INFO, which prints on failure
    // alone: a benchmark whose numbers are invisible on a green run is one
    // nobody can cite from without re-running it under a debugger.
    std::cout << "morph_bench: " << trials << " trial(s), " << latencySamples << " serial round trips + a "
              << windowMs.count() << " ms window per concurrency point each\n"
              << "latency ms          best      median       worst\n";
    auto latencyRow = [](const char* name, const std::vector<double>& values) {
        std::cout << "  " << name << "        " << *std::ranges::min_element(values) << "    " << medianOf(values)
                  << "    " << *std::ranges::max_element(values) << "\n";
    };
    latencyRow("p50", p50s);
    latencyRow("p95", p95s);
    latencyRow("p99", p99s);
    std::cout << "executes/sec        best      median       worst\n";
    for (std::size_t slot = 0; slot < kConcurrencies.size(); ++slot) {
        const std::vector<double>& values = throughputColumns.at(slot);
        std::cout << "  concurrency " << kConcurrencies.at(slot) << "    " << *std::ranges::max_element(values)
                  << "    " << medianOf(values) << "    " << *std::ranges::min_element(values) << "\n";
    }

    INFO("p50=" << p50Best << "ms p95=" << p95Best << "ms p99=" << p99Best << "ms (best of " << trials << " trials)");

    // The `p50_ms`/`p95_ms`/`p99_ms`/`throughput` keys are the ones the
    // previous schema had and are kept, so an archived run from before this
    // change still diffs against one from after; they now carry the best
    // trial rather than the only one. Everything else is additive.
    std::ofstream artifact{std::string{BENCH_ARTIFACT_DIR} + "/bench_dispatch_latency.json"};
    artifact << "{\"trials\":" << trials << ",\"latency_samples_per_trial\":" << latencySamples
             << ",\"throughput_window_ms\":" << windowMs.count() << ",\"p50_ms\":" << p50Best
             << ",\"p95_ms\":" << p95Best << ",\"p99_ms\":" << p99Best << ",\"p99_ms_median_trial\":" << medianOf(p99s)
             << ",\"p99_ms_worst_trial\":" << p99Worst << ",\"throughput\":[";
    for (std::size_t slot = 0; slot < kConcurrencies.size(); ++slot) {
        if (slot > 0) {
            artifact << ",";
        }
        artifact << "{\"concurrency\":" << kConcurrencies.at(slot)
                 << ",\"executes_per_sec\":" << *std::ranges::max_element(throughputColumns.at(slot))
                 << ",\"executes_per_sec_worst_trial\":" << *std::ranges::min_element(throughputColumns.at(slot))
                 << "}";
    }
    artifact << "],\"trial_detail\":[";
    for (std::size_t i = 0; i < results.size(); ++i) {
        if (i > 0) {
            artifact << ",";
        }
        artifact << "{\"p50_ms\":" << results[i].p50Ms << ",\"p95_ms\":" << results[i].p95Ms
                 << ",\"p99_ms\":" << results[i].p99Ms << ",\"executes_per_sec\":[";
        for (std::size_t slot = 0; slot < kConcurrencies.size(); ++slot) {
            if (slot > 0) {
                artifact << ",";
            }
            artifact << results[i].throughput.at(slot);
        }
        artifact << "]}";
    }
    artifact << "]}";

    CHECK(p99Best <= p99MsMax);
    CHECK(*std::ranges::max_element(throughputColumns.at(0)) >= minThroughput);
}

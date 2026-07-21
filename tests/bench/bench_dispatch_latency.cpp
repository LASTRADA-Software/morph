// SPDX-License-Identifier: Apache-2.0

// Throughput + latency benchmark for morph::backend::RemoteServer's dispatch
// hot path, against a trivial echo model (measures framework overhead, not
// business logic). See docs/spec/testing_strategy.md.
//
// Opt-in: built only under -DMORPH_BUILD_LOAD_TESTS=ON, never part of the
// default `morph_tests` target. Writes a JSON artifact to
// BENCH_ARTIFACT_DIR/bench_dispatch_latency.json (the build directory) so CI
// can archive successive runs and diff them for regressions; also enforces a
// configurable regression gate via REQUIRE on p99 latency and minimum
// concurrency-1 throughput.
//
// Override the regression thresholds with:
//   MORPH_BENCH_P99_MS_MAX=<double>          (default: 50.0)
//   MORPH_BENCH_MIN_THROUGHPUT=<double>      (default: 500.0, executes/sec at concurrency=1)

#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <memory>
#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <morph/core/wire.hpp>
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

// Nearest-rank percentile over an already-sorted sample -- adequate for a
// benchmark's regression gate, not a statistically rigorous estimator.
double percentile(std::vector<double>& sortedMs, double p) {
    if (sortedMs.empty()) {
        return 0.0;
    }
    auto rank = static_cast<std::size_t>(p * static_cast<double>(sortedMs.size() - 1));
    return sortedMs[rank];
}

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
    const double p99MsMax = envDoubleOr("MORPH_BENCH_P99_MS_MAX", 50.0);
    const double minThroughput = envDoubleOr("MORPH_BENCH_MIN_THROUGHPUT", 500.0);

    morph::exec::ThreadPoolExecutor pool{4};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);

    morph::testing::WaitReply regWaiter;
    server->handle(morph::wire::encode(morph::wire::makeRegister("Bench_EchoModel")), std::ref(regWaiter));
    REQUIRE(regWaiter.await());
    REQUIRE(regWaiter.env.kind == "ok");
    const uint64_t modelId = regWaiter.env.modelId;

    morph::wire::Envelope req;
    req.kind = "execute";
    req.modelId = modelId;
    req.modelType = "Bench_EchoModel";
    req.actionType = "Bench_EchoAction";
    req.body = R"({"s":"hello"})";

    // ── Phase A: serial (concurrency=1) latency distribution ────────────────
    constexpr int latencySamples = 2000;
    std::vector<double> latenciesMs;
    latenciesMs.reserve(latencySamples);
    uint64_t nextCallId = 1;
    for (int i = 0; i < latencySamples; ++i) {
        req.callId = nextCallId++;
        morph::testing::WaitReply waiter;
        const auto start = std::chrono::steady_clock::now();
        server->handle(morph::wire::encode(req), std::ref(waiter));
        REQUIRE(waiter.await());
        const auto end = std::chrono::steady_clock::now();
        REQUIRE(waiter.env.kind == "ok");
        latenciesMs.push_back(std::chrono::duration<double, std::milli>(end - start).count());
    }
    std::ranges::sort(latenciesMs);
    const double p50 = percentile(latenciesMs, 0.50);
    const double p95 = percentile(latenciesMs, 0.95);
    const double p99 = percentile(latenciesMs, 0.99);

    // ── Phase B: throughput at increasing concurrency ────────────────────────
    struct ThroughputPoint {
        int concurrency;
        double executesPerSec;
    };
    std::vector<ThroughputPoint> throughput;
    for (int concurrency : {1, 2, 4, 8, 16}) {
        std::atomic<int> inFlight{0};
        std::atomic<uint64_t> completed{0};
        const auto windowStart = std::chrono::steady_clock::now();
        const auto windowEnd = windowStart + 500ms;
        while (std::chrono::steady_clock::now() < windowEnd) {
            if (inFlight.load(std::memory_order_relaxed) < concurrency) {
                inFlight.fetch_add(1, std::memory_order_relaxed);
                morph::wire::Envelope call = req;
                call.callId = nextCallId++;
                server->handle(morph::wire::encode(call), [&](const std::string&) {
                    completed.fetch_add(1, std::memory_order_relaxed);
                    inFlight.fetch_sub(1, std::memory_order_relaxed);
                });
            } else {
                std::this_thread::yield();
            }
        }
        REQUIRE(morph::testing::waitUntil([&] { return inFlight.load() == 0; }, 5000ms));
        const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - windowStart).count();
        throughput.push_back({concurrency, static_cast<double>(completed.load()) / elapsed});
    }

    // ── Report + artifact ────────────────────────────────────────────────────
    INFO("p50=" << p50 << "ms p95=" << p95 << "ms p99=" << p99 << "ms");
    for (const auto& point : throughput) {
        INFO("concurrency=" << point.concurrency << " -> " << point.executesPerSec << " executes/sec");
    }

    std::ofstream artifact{std::string{BENCH_ARTIFACT_DIR} + "/bench_dispatch_latency.json"};
    artifact << "{\"p50_ms\":" << p50 << ",\"p95_ms\":" << p95 << ",\"p99_ms\":" << p99 << ",\"throughput\":[";
    for (std::size_t i = 0; i < throughput.size(); ++i) {
        if (i > 0) {
            artifact << ",";
        }
        artifact << "{\"concurrency\":" << throughput[i].concurrency
                 << ",\"executes_per_sec\":" << throughput[i].executesPerSec << "}";
    }
    artifact << "]}";

    CHECK(p99 <= p99MsMax);
    CHECK(throughput.front().executesPerSec >= minThroughput);
}

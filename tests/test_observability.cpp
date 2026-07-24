// SPDX-License-Identifier: Apache-2.0

#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <morph/core/observability.hpp>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using ObserveGuard = morph::observe::ScopedObserveOverride;

// ── morph::observe::setMetricSink / metricsEnabled ────────────────────────────

TEST_CASE("morph::observe::metricsEnabled: false with no sink installed", "[observability]") {
    ObserveGuard guard;
    morph::observe::setMetricSink(nullptr);
    REQUIRE_FALSE(morph::observe::metricsEnabled());
}

TEST_CASE("morph::observe::setMetricSink: installing a sink enables metricsEnabled", "[observability]") {
    ObserveGuard guard;
    morph::observe::setMetricSink([](const morph::observe::MetricEvent&) {});
    REQUIRE(morph::observe::metricsEnabled());
}

TEST_CASE("morph::observe::detail::emitMetric: sink receives metric, value, and tags", "[observability]") {
    ObserveGuard guard;
    morph::observe::Metric capturedMetric = morph::observe::Metric::executeErrors;
    double capturedValue = 0.0;
    std::vector<std::pair<std::string, std::string>> capturedTags;

    morph::observe::setMetricSink([&](const morph::observe::MetricEvent& evt) {
        capturedMetric = evt.metric;
        capturedValue = evt.value;
        for (auto& [key, val] : evt.tags) {
            capturedTags.emplace_back(std::string{key}, std::string{val});
        }
    });

    std::array<std::pair<std::string_view, std::string_view>, 1> const tags{{{"modelType", "Account"}}};
    morph::observe::detail::emitMetric(morph::observe::Metric::executeLatencyMs, 12.5, tags);

    REQUIRE(capturedMetric == morph::observe::Metric::executeLatencyMs);
    REQUIRE(capturedValue == 12.5);
    REQUIRE(capturedTags.size() == 1);
    REQUIRE(capturedTags[0].first == "modelType");
    REQUIRE(capturedTags[0].second == "Account");
}

TEST_CASE("morph::observe::detail::emitMetric: no sink installed is a silent no-op", "[observability]") {
    ObserveGuard guard;
    morph::observe::setMetricSink(nullptr);
    morph::observe::detail::emitMetric(morph::observe::Metric::registerCount, 1.0);
    REQUIRE_FALSE(morph::observe::metricsEnabled());
}

// ── morph::observe::setTraceSink / detail::beginSpan / detail::endSpan ───────

TEST_CASE("morph::observe::detail::beginSpan: returns 0 sentinel with no trace sink installed", "[observability]") {
    ObserveGuard guard;
    morph::observe::setTraceSink({});
    REQUIRE(morph::observe::detail::beginSpan("req-1", "Account", "Deposit") == 0);
}

TEST_CASE("morph::observe::setTraceSink: beginSpan/endSpan pair carries id and ok flag", "[observability]") {
    ObserveGuard guard;
    std::string capturedRequestId;
    std::string capturedModelType;
    std::string capturedActionType;
    morph::observe::SpanId endedId = 0;
    bool endedOk = false;

    morph::observe::setTraceSink(morph::observe::TraceSink{
        .beginSpan =
            [&](std::string_view requestId, std::string_view modelType, std::string_view actionType) {
                capturedRequestId = requestId;
                capturedModelType = modelType;
                capturedActionType = actionType;
                return morph::observe::SpanId{42};
            },
        .endSpan =
            [&](morph::observe::SpanId id, bool ok) {
                endedId = id;
                endedOk = ok;
            },
    });

    auto const spanId = morph::observe::detail::beginSpan("req-1", "Account", "Deposit");
    REQUIRE(spanId == 42);
    REQUIRE(capturedRequestId == "req-1");
    REQUIRE(capturedModelType == "Account");
    REQUIRE(capturedActionType == "Deposit");

    morph::observe::detail::endSpan(spanId, true);
    REQUIRE(endedId == 42);
    REQUIRE(endedOk);
}

TEST_CASE("morph::observe::setTraceSink: a sink missing either callback stays disabled", "[observability]") {
    ObserveGuard guard;
    morph::observe::setTraceSink(morph::observe::TraceSink{
        .beginSpan = [](std::string_view, std::string_view, std::string_view) { return morph::observe::SpanId{7}; },
        .endSpan = nullptr,
    });
    REQUIRE(morph::observe::detail::beginSpan("req", "M", "A") == 0);
}

TEST_CASE("morph::observe::detail::beginSpan: falls back to the sentinel if traceOn is set without a beginSpan hook",
          "[observability]") {
    ObserveGuard guard;
    // setTraceSink() only ever sets traceOn together with a fully-populated
    // sink, so this combination is unreachable through the public API --
    // reach into the singleton directly to exercise detail::beginSpan's own
    // defensive check of state.traceSink.beginSpan.
    auto& state = morph::observe::detail::observeState();
    {
        std::scoped_lock const lock{state.traceMtx};
        state.traceSink = morph::observe::TraceSink{};
    }
    state.traceOn.store(true, std::memory_order_relaxed);
    REQUIRE(morph::observe::detail::beginSpan("req", "M", "A") == 0);
}

TEST_CASE("morph::observe::detail::endSpan: sentinel id 0 is always a no-op", "[observability]") {
    ObserveGuard guard;
    bool endCalled = false;
    morph::observe::setTraceSink(morph::observe::TraceSink{
        .beginSpan = [](std::string_view, std::string_view, std::string_view) { return morph::observe::SpanId{1}; },
        .endSpan = [&](morph::observe::SpanId, bool) { endCalled = true; },
    });
    morph::observe::detail::endSpan(0, true);
    REQUIRE_FALSE(endCalled);
}

// ── morph::observe::ScopedObserveOverride ─────────────────────────────────────

TEST_CASE("morph::observe::ScopedObserveOverride: restores previous sinks on scope exit", "[observability]") {
    morph::observe::setMetricSink(nullptr);
    morph::observe::setTraceSink({});
    REQUIRE_FALSE(morph::observe::metricsEnabled());
    {
        ObserveGuard guard;
        morph::observe::setMetricSink([](const morph::observe::MetricEvent&) {});
        morph::observe::setTraceSink(morph::observe::TraceSink{
            .beginSpan = [](std::string_view, std::string_view,
                            std::string_view) { return morph::observe::SpanId{1}; },
            .endSpan = [](morph::observe::SpanId, bool) {},
        });
        REQUIRE(morph::observe::metricsEnabled());
    }
    REQUIRE_FALSE(morph::observe::metricsEnabled());
    REQUIRE(morph::observe::detail::beginSpan("r", "m", "a") == 0);
}

// ── Thread safety ─────────────────────────────────────────────────────────────

TEST_CASE("concurrent metric emission is thread-safe", "[observability]") {
    ObserveGuard guard;
    std::atomic<int> count{0};
    morph::observe::setMetricSink(
        [&](const morph::observe::MetricEvent&) { count.fetch_add(1, std::memory_order_relaxed); });

    constexpr int numThreads = 8;
    constexpr int emitsPerThread = 200;
    std::vector<std::thread> threads;
    threads.reserve(numThreads);
    for (int i = 0; i < numThreads; ++i) {
        threads.emplace_back([&] {
            for (int j = 0; j < emitsPerThread; ++j) {
                morph::observe::detail::emitMetric(morph::observe::Metric::executeLatencyMs, 1.0);
            }
        });
    }
    for (auto& thr : threads) {
        thr.join();
    }
    REQUIRE(count.load() == numThreads * emitsPerThread);
}

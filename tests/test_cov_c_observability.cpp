// SPDX-License-Identifier: Apache-2.0
//
// The observability seams when a sink is missing, half set, or throws: nothing is reported and nothing escapes.

#include <catch2/catch_test_macros.hpp>
#include <morph/core/observability.hpp>
#include <stdexcept>
#include <string_view>

namespace obs = morph::observe;

TEST_CASE("ending a span with no sink, or a sink without an end, reports nothing", "[covc][core][observability]") {
    obs::ScopedObserveOverride const scope;
    obs::setTraceSink(obs::TraceSink{});
    obs::detail::endSpan(7, true);
    CHECK(obs::detail::beginSpan("r", "m", "a") == 0);

    int began = 0;
    obs::setTraceSink(obs::TraceSink{.beginSpan =
                                         [&began](std::string_view, std::string_view, std::string_view) {
                                             ++began;
                                             return obs::SpanId{9};
                                         },
                                     .endSpan = nullptr});
    obs::detail::endSpan(7, true);
    CHECK(obs::detail::beginSpan("r", "m", "a") == 0);
    CHECK(began == 0);
}

TEST_CASE("a sink that throws yields no span and no error", "[covc][core][observability]") {
    obs::ScopedObserveOverride const scope;
    int ended = 0;
    obs::setTraceSink(
        obs::TraceSink{.beginSpan = [](std::string_view, std::string_view, std::string_view) -> obs::SpanId {
                           throw std::runtime_error{"begin"};
                       },
                       .endSpan =
                           [&ended](obs::SpanId, bool) {
                               ++ended;
                               throw std::runtime_error{"end"};
                           }});
    CHECK(obs::detail::beginSpan("r", "m", "a") == 0);
    obs::detail::endSpan(5, false);
    CHECK(ended == 1);
    obs::detail::endSpan(0, false);
    CHECK(ended == 1);
    obs::setMetricSink([](obs::MetricEvent const&) { throw std::runtime_error{"metric"}; });
    obs::detail::emitMetric(obs::Metric::executeErrors, 1.0);
}

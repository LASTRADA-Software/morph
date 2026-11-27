// SPDX-License-Identifier: Apache-2.0
//
// Compile-check fixture for MORPH_ENABLE_OBSERVABILITY=OFF (see the try_compile()
// block in tests/CMakeLists.txt). Each probe installs a sink the way a host
// does; PROBE_METRIC_SINK, PROBE_TRACE_SINK and PROBE_SCOPED_OVERRIDE pick which.
// It must compile when MORPH_OBSERVABILITY_DISABLED is not defined, and must be
// refused, naming the option, when it is.

#include <morph/core/observability.hpp>

void installSink() {
#ifdef PROBE_TRACE_SINK
    morph::observe::setTraceSink(morph::observe::TraceSink{});
#elif defined(PROBE_SCOPED_OVERRIDE)
    morph::observe::ScopedObserveOverride const guard;
    (void)guard;
#else
    morph::observe::setMetricSink([](const morph::observe::MetricEvent&) {});
#endif
}

// SPDX-License-Identifier: Apache-2.0
//
// Compile-check fixture for MORPH_ENABLE_OBSERVABILITY=OFF (see the try_compile()
// block in tests/CMakeLists.txt). Uses everything a compiled-out build still
// offers: it must compile with MORPH_OBSERVABILITY_DISABLED defined and with
// it undefined, so the refusal of the sink setters is the only thing the macro
// changes for a caller.

#include <morph/core/observability.hpp>

#include <string_view>

bool emitEverything() {
    morph::observe::detail::emitMetric(morph::observe::Metric::executeErrors, 1.0);
    auto const span = morph::observe::detail::beginSpan(std::string_view{}, std::string_view{}, std::string_view{});
    morph::observe::detail::endSpan(span, true);
    return morph::observe::metricsEnabled();
}

// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>
#include <cstdint>
#include <morph/session/session.hpp>
#include <morph/util/datetime.hpp>

#include "clock.hpp"
#include "errors.hpp"

/// @file
/// The two things every model in this rung needs and morph does not provide:
/// the principal check, and the conversion between the ladder's injectable
/// clock (`morph::ladder::now()`, examples/common/clock.hpp) and the
/// epoch-millisecond integers the database columns hold.

namespace crm {

/// @brief Throws unless a non-empty principal is authenticated.
///
/// Called by every mutating action rather than assumed — crm's field-level
/// audit history (README build order §6) needs a real author on every entry.
/// @throws EmptyPrincipalError when no principal, or an empty one, is in scope.
inline void requirePrincipal() {
    const auto* ctx = ::morph::session::current();
    if (ctx == nullptr || ctx->principal.empty()) {
        throw EmptyPrincipalError{};
    }
}

/// @brief The current instant in epoch milliseconds, honouring a test's
///        `morph::ladder::ScopedClockOverride`.
/// @return Milliseconds since the Unix epoch.
[[nodiscard]] inline std::int64_t nowMillis() {
    return (*::morph::ladder::now().value).value.time_since_epoch().count();
}

}  // namespace crm

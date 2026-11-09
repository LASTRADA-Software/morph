// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>
#include <cstdint>
#include <morph/util/datetime.hpp>

#include "clock.hpp"

/// @file
/// The three things every model in this rung needs and morph does not
/// provide: the principal check, and the two conversions between the ladder's
/// injectable clock (`morph::ladder::now()`, examples/common/clock.hpp) and
/// the epoch-millisecond integers the database columns hold.
///
/// Epoch milliseconds is the storage form, not the DTO form: a `SampleView`
/// carries a `morph::time::Timestamp` (examples/IMPLEMENTATION.md rule 3),
/// and the integer never escapes the DTO⇄entity mapping.

namespace lims {

/// @brief Throws unless a non-empty principal is authenticated.
///
/// Called by every mutating action rather than assumed: the README names
/// empty-principal audit entries as disqualifying for this rung, and an
/// authorizer that ran is not the same fact as a principal that is present
/// (the authorize/authenticate TOCTOU it calls out).
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

/// @brief Wraps an epoch-millisecond column value as a wire `Timestamp`.
/// @param millis Milliseconds since the Unix epoch.
/// @return The engaged timestamp naming that instant.
[[nodiscard]] inline ::morph::time::Timestamp timestampFromMillis(std::int64_t millis) {
    return ::morph::time::Timestamp{
        ::morph::time::DateTime{std::chrono::sys_time<std::chrono::milliseconds>{std::chrono::milliseconds{millis}}}};
}

}  // namespace lims

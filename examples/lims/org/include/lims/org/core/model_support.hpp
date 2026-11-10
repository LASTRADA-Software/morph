// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Lightweight/SqlError.hpp>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <morph/session/session.hpp>
#include <morph/util/datetime.hpp>
#include <string>
#include <string_view>

#include "clock.hpp"
#include "lims/org/core/errors.hpp"

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

/// @brief Whether @p error is SQLite's single writer turning this one away.
///
/// The driver reports SQLite's primary result code as the native code: 5 for busy, 6 for locked. The message is the
/// fallback for a driver that reports neither, and is limited to those two words: any other failure (a constraint, a
/// missing table) is a defect and must surface as itself.
/// @param error The failure.
/// @return `true` when the database was busy or locked.
[[nodiscard]] inline bool isBusy(::Lightweight::SqlException const& error) {
    constexpr int kSqliteBusy = 5;
    constexpr int kSqliteLocked = 6;
    constexpr unsigned int kPrimaryCodeMask = 0xFFU;
    unsigned int const code = static_cast<unsigned int>(error.info().nativeErrorCode) & kPrimaryCodeMask;
    if (code == kSqliteBusy || code == kSqliteLocked) {
        return true;
    }
    std::string text = error.what();
    std::ranges::transform(text, text.begin(),
                           [](unsigned char letter) { return static_cast<char>(std::tolower(letter)); });
    return text.contains("database is locked") || text.contains("database table is locked") || text.contains("busy");
}

/// @brief The authenticated principal, or empty when there is none. A list answers an empty principal with nothing,
///        since nobody holds a role in any scope.
/// @return The principal.
[[nodiscard]] inline std::string currentPrincipal() {
    const auto* ctx = ::morph::session::current();
    return ctx == nullptr ? std::string{} : ctx->principal;
}

/// @brief The principal namespace reserved for the server's own clients; no sign-in yields a principal in it.
inline constexpr std::string_view kServicePrincipalPrefix = "system:";

/// @brief Whether @p principal is one of the server's own clients.
/// @param principal The principal.
/// @return `true` for `system:<name>` with a non-empty name.
[[nodiscard]] inline bool isServicePrincipal(std::string_view principal) noexcept {
    return principal.starts_with(kServicePrincipalPrefix) && principal.size() > kServicePrincipalPrefix.size();
}

/// @brief Throws `Forbidden` unless the authenticated principal is one of the server's own clients.
/// @param action The action, for the refusal.
/// @throws Forbidden when it is not.
inline void requireServicePrincipal(std::string_view action) {
    if (!isServicePrincipal(currentPrincipal())) {
        throw Forbidden{std::string{action} + ": only the server's own clients send it"};
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

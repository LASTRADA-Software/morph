// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <memory>
#include <morph/session/session_auth.hpp>
#include <string_view>

/// @file
/// Kanban's `IAuthorizer` -- `SigningAuthorizer`-derived, mirroring
/// `bookmarks::auth::BookmarksAuthorizer`'s shape (design spec §3's
/// corrected identity decision, *not* `polls::auth::PollsAuthorizer`'s
/// `AllowAllAuthorizer`-derived shape): `BoardModel::requireRole()` reads
/// `session::current()->principal` to key its `project_has_roles` lookup,
/// and only a verifying authorizer supplies a trustworthy one --
/// `security.md`'s documented behavior clears an unauthenticated caller's
/// principal to empty before every remote dispatch, which would make every
/// role check either always deny or silently diverge between `Local` and
/// `Socket` test modes.
///
/// `authorizeRegister`/`authorizeInstance` are left at their inherited
/// permissive defaults: `BoardModel` has no per-instance owner concept (its
/// instances are shared/keyed by `projectId`, exactly like `PollModel`), and
/// the actual role gate lives entirely inside `BoardModel::execute()`/
/// `ProjectAdminModel::execute()` via `requireRole()`.

namespace kanban::auth {

/// @brief Namespace prefix reserved for service principals. No human may log
///        in under it -- see `isReservedPrincipal`. Mirrors
///        `bookmarks::auth::kServicePrincipalPrefix`; kanban has no service
///        worker of its own yet, but `AuthModel::execute(const Login&)`
///        still refuses to mint a token in this namespace on request, same
///        as bookmarks, as a defense-in-depth measure that costs nothing to
///        keep even before a concrete service principal exists.
inline constexpr std::string_view kServicePrincipalPrefix = "system:";

/// @brief Longest principal this rung accepts, in bytes.
inline constexpr std::size_t kMaxPrincipalBytes = 64;

/// @brief Whether @p principal is acceptable as a login identity for this
///        rung. Mirrors `bookmarks::auth::isValidPrincipal` exactly (see that
///        function's own doc comment for the full rationale): non-empty, at
///        most `kMaxPrincipalBytes` long, ASCII letters/digits/`.`/`_`/`:`/`-`
///        only.
/// @param principal Candidate principal string.
/// @return `true` if @p principal is non-empty, at most `kMaxPrincipalBytes`
///         long, and every byte is an ASCII letter, digit, `.`, `_`, `:`, or `-`.
[[nodiscard]] inline bool isValidPrincipal(std::string_view principal) noexcept {
    if (principal.empty() || principal.size() > kMaxPrincipalBytes) {
        return false;
    }
    for (const char ch : principal) {
        const auto byte = static_cast<unsigned char>(ch);
        const bool ok = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9') ||
                        byte == '.' || byte == '_' || byte == '-' || byte == ':';
        if (!ok) {
            return false;
        }
    }
    return true;
}

/// @brief Whether @p principal is reserved for the server's own internal
///        workers and must never be handed to a caller. Mirrors
///        `bookmarks::auth::isReservedPrincipal` exactly.
/// @param principal Candidate principal string.
/// @return `true` if @p principal begins with `kServicePrincipalPrefix`.
[[nodiscard]] inline bool isReservedPrincipal(std::string_view principal) noexcept {
    return principal.starts_with(kServicePrincipalPrefix);
}

/// @brief This rung's `IAuthorizer`: verifies HMAC-signed session tokens
///        (inherited `SigningAuthorizer::authorize`/`authenticate`), stays
///        permissive on register/instance admission.
class KanbanAuthorizer : public ::morph::session::SigningAuthorizer {
public:
    using SigningAuthorizer::SigningAuthorizer;
};

/// @brief Installs the process-wide `TokenIssuer` `Login` mints tokens from.
/// @param issuer The issuer to install, or `nullptr` to clear it.
void setTokenIssuer(std::shared_ptr<::morph::session::TokenIssuer> issuer);

/// @brief Returns the process-wide `TokenIssuer` installed by
///        `setTokenIssuer`, or `nullptr` if none is installed yet.
[[nodiscard]] std::shared_ptr<::morph::session::TokenIssuer> tokenIssuer();

}  // namespace kanban::auth

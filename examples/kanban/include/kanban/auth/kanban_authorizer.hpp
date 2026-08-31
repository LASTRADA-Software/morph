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

    /// @brief Model type id of the one model a tokenless caller may execute on.
    static constexpr std::string_view kAnonymousModelType = "AuthModel";
    /// @brief Action type id of the one action a tokenless caller may execute.
    static constexpr std::string_view kAnonymousActionType = "Login";

    /// @brief `SigningAuthorizer::authorize`, with exactly one carve-out:
    ///        `AuthModel`/`Login` is admitted without a token.
    ///
    /// The same chicken-and-egg deadlock `bookmarks::auth::BookmarksAuthorizer`
    /// documents and closes, in the rung that inherited its shape but not its
    /// carve-out: `SigningAuthorizer::authorize()` verifies `Context::token` on
    /// **every** `execute` and returns `false` when there is none -- including
    /// for `Login`, which is the only way to obtain a token. Without this,
    /// every action a fresh remote client can send is answered
    /// `err "unauthorized"`, login included, and `ladder_kanban_server` is
    /// unusable to any client that does not already hold a token minted out of
    /// band. `ladder_kanban_headless` says so in as many words: it takes the
    /// token on its command line, because it cannot ask the server for one.
    ///
    /// Found the same way bookmarks' was -- by driving a real
    /// `ladder_kanban_server` from an out-of-process client
    /// (`scripts/scenario/`). Nothing had exercised `Login` *over a server*
    /// before: `tests/journeys/test_kanban_journeys.cpp` calls
    /// `AuthModel::execute()` directly, which never consults an authorizer,
    /// and then installs a token it mints itself with the rung's own
    /// `TokenIssuer`.
    ///
    /// The carve-out is as narrow as it can be -- one model type, one action
    /// type, both compared exactly -- and gives away nothing that was not
    /// already reachable. `AuthModel` is stateless, holds no database, and
    /// `execute(const Login&)` rejects an invalid principal and refuses the
    /// reserved `system:` namespace outright, so an anonymous caller can mint
    /// a token for a username it names and nothing more, which is what a
    /// dev-mode login *is*. Every other model and action still requires a
    /// validly signed, unexpired token; in particular `BoardModel::
    /// requireRole()` still reads a principal `RemoteServer` only stamps when
    /// `authenticate()` vouches for it, so no role check is weakened.
    ///
    /// @param ctx        Per-call session (its `token` is verified for
    ///                   everything but the carve-out).
    /// @param modelType  Target model type id.
    /// @param actionType Target action type id.
    /// @return `true` to allow dispatch, `false` to reject.
    [[nodiscard]] bool authorize(const ::morph::session::Context& ctx,
                                 // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
                                 std::string_view modelType, std::string_view actionType) const override {
        if (modelType == kAnonymousModelType && actionType == kAnonymousActionType) {
            return true;
        }
        return SigningAuthorizer::authorize(ctx, modelType, actionType);
    }
};

/// @brief Installs the process-wide `TokenIssuer` `Login` mints tokens from.
/// @param issuer The issuer to install, or `nullptr` to clear it.
void setTokenIssuer(std::shared_ptr<::morph::session::TokenIssuer> issuer);

/// @brief Returns the process-wide `TokenIssuer` installed by
///        `setTokenIssuer`, or `nullptr` if none is installed yet.
[[nodiscard]] std::shared_ptr<::morph::session::TokenIssuer> tokenIssuer();

}  // namespace kanban::auth

// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <memory>
#include <morph/session/session_auth.hpp>
#include <mutex>
#include <string>
#include <string_view>

#include "ledger/core/types.hpp"

/// @file
/// This rung's one `IAuthorizer`. Mirrors
/// `bookmarks::auth::BookmarksAuthorizer`
/// (`examples/bookmarks/include/bookmarks/auth/bookmarks_authorizer.hpp`)
/// closely: real signed-token authentication via the inherited
/// `SigningAuthorizer::authorize`/`authenticate`, plus a `Login` carve-out so
/// a fresh client can obtain a token at all, and the two instance-lifecycle
/// hooks left at their permissive defaults for the same reason bookmarks'
/// are -- every mutating action still re-checks `session::current()->
/// principal` itself (`docs/spec/security.md` rule: "models must re-check
/// their own preconditions and authorization"), so gating `register`/attach
/// by identity here would buy nothing `LedgerModel`'s own checks don't
/// already provide, and `LedgerModel`'s instances are keyed by `ledgerId`
/// (shared across every client that opens the same book, `BRIDGE_KEY_FROM`
/// in `ledger_model.hpp`), which has no single owning caller for
/// `authorizeInstance` to compare against -- the identical shape
/// `PollsAuthorizer` documents for `PollModel`'s keyed instances.
///
/// What is genuinely enforced -- the whole trust boundary this rung claims --
/// is: `SigningAuthorizer::authorize()` verifying a real signed token on
/// every `execute` other than `Login`, `RemoteServer` overwriting
/// `Context::principal` with the verified identity before the model runs,
/// and each mutating model action refusing an empty principal
/// (`EmptyPrincipalError`, design spec §11) or, for `RunReportJob`, refusing
/// any principal but `kReportRunnerPrincipal`.

namespace ledger::auth {

/// @brief Longest principal this rung accepts, in bytes. Same bound as
///        `bookmarks::auth::kMaxPrincipalBytes` -- this is a *username*, not
///        free text.
inline constexpr std::size_t kMaxPrincipalBytes = 64;

/// @brief Namespace prefix reserved for service principals --
///        `kReportRunnerPrincipal` (`ledger/core/types.hpp`) is the only one
///        today. No human may log in under it -- see `isReservedPrincipal`.
inline constexpr std::string_view kServicePrincipalPrefix = "system:";

/// @brief Whether @p principal is acceptable as a login identity for this
///        rung.
///
/// ASCII-only and short, mirroring `bookmarks::auth::isValidPrincipal`
/// exactly: `[A-Za-z0-9._:-]`, non-empty, at most `kMaxPrincipalBytes` long.
/// `:` is included so `kReportRunnerPrincipal` ("system:report-runner")
/// itself would pass this check -- `isReservedPrincipal` is what actually
/// keeps a client from claiming it, not the charset.
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
///        report runner and must never be handed to a caller.
///
/// Without this check a client could log in as `kReportRunnerPrincipal` and
/// obtain a genuinely-signed token for it, which `LedgerModel::execute(const
/// RunReportJob&)` accepts -- letting an ordinary user complete report jobs
/// the runner owns and, since that check is the only thing distinguishing
/// the runner from a user, collapsing the layering boundary
/// `kReportRunnerPrincipal`'s own doc comment describes. Mirrors
/// `bookmarks::auth::isReservedPrincipal`.
/// @param principal Candidate principal string.
/// @return `true` if @p principal begins with `kServicePrincipalPrefix`.
[[nodiscard]] inline bool isReservedPrincipal(std::string_view principal) noexcept {
    return principal.starts_with(kServicePrincipalPrefix);
}

/// @brief This rung's `IAuthorizer`: real signed-token auth plus a `Login`
///        carve-out. See this file's `@file` comment for the full rationale.
class LedgerAuthorizer : public ::morph::session::SigningAuthorizer {
public:
    using SigningAuthorizer::SigningAuthorizer;

    /// @brief Model type id of the one model a tokenless caller may execute on.
    static constexpr std::string_view kAnonymousModelType = "AuthModel";
    /// @brief Action type id of the one action a tokenless caller may execute.
    static constexpr std::string_view kAnonymousActionType = "Login";

    /// @brief `SigningAuthorizer::authorize`, with exactly one carve-out:
    ///        `AuthModel`/`Login` is admitted without a token.
    ///
    /// Without this the rung has the identical chicken-and-egg deadlock
    /// `BookmarksAuthorizer::authorize`'s doc comment describes:
    /// `SigningAuthorizer::authorize()` verifies `Context::token` on every
    /// `execute` and returns `false` when there is none -- including for
    /// `Login`, the only way to obtain a token in the first place. The
    /// carve-out is as narrow as it can be -- one model type, one action
    /// type, both compared exactly -- and gives away nothing that was not
    /// already reachable: `AuthModel` is stateless, holds no database, and
    /// `execute(const Login&)`'s own body rejects an invalid principal and
    /// the reserved `system:` namespace outright. Every other model and
    /// every other action still requires a validly signed, unexpired token.
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

namespace detail {

/// @brief Backing storage for `setTokenIssuer`/`tokenIssuer` -- a single
///        shared slot, guarded by a single mutex. Mirrors
///        `bookmarks::auth::detail`'s identical pair.
[[nodiscard]] inline std::mutex& tokenIssuerMutex() {
    static std::mutex mtx;
    return mtx;
}

[[nodiscard]] inline std::shared_ptr<::morph::session::TokenIssuer>& tokenIssuerSlot() {
    static std::shared_ptr<::morph::session::TokenIssuer> slot;
    return slot;
}

}  // namespace detail

/// @brief Installs @p issuer as the process-global `TokenIssuer`. `AuthModel`
///        is registered via the plain `BRIDGE_REGISTER_MODEL`
///        default-construction path rather than `ModelRegistryFactory`'s
///        per-instance construction-hook seam, so a process-global slot
///        passes the secret through instead -- the same answer
///        `bookmarks::auth::setTokenIssuer` and `morph::journal::
///        setActionLog` already use. `App` calls this once at startup with
///        the *same* secret it hands to `LedgerAuthorizer`, so a token
///        `AuthModel::execute(const Login&)` mints verifies against the very
///        authorizer that will check every subsequent call.
/// @param issuer The issuer every `AuthModel` instance will read, or
///        `nullptr` to clear it.
inline void setTokenIssuer(std::shared_ptr<::morph::session::TokenIssuer> issuer) {
    const std::scoped_lock lock{detail::tokenIssuerMutex()};
    detail::tokenIssuerSlot() = std::move(issuer);
}

/// @brief Returns the process-global `TokenIssuer` installed by
///        `setTokenIssuer`, or `nullptr` if none is installed yet.
[[nodiscard]] inline std::shared_ptr<::morph::session::TokenIssuer> tokenIssuer() {
    const std::scoped_lock lock{detail::tokenIssuerMutex()};
    return detail::tokenIssuerSlot();
}

}  // namespace ledger::auth

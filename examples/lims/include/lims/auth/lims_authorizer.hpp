// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <functional>
#include <morph/session/session.hpp>
#include <morph/session/session_auth.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "lims/dto/verification_dto.hpp"

/// @file
/// This rung's one `IAuthorizer`: the coarse, type-level half of the
/// four-eyes control (README build order §6).
///
/// @par What it can and cannot do, exactly
/// `IAuthorizer::authorize` is handed the caller's `Context` plus the target
/// **model type** and **action type** ids — and nothing else. It can answer
/// "may this principal ever verify a result?"; it structurally cannot answer
/// "may this principal verify *this* result?", because it never sees the
/// result. The second question is the one four-eyes actually turns on
/// (a verifier must not verify their own reading), so `SampleModel` enforces
/// it itself. Neither check stands in for the other.
///
/// @par Why it derives from `SigningAuthorizer`
/// A role gate is only worth anything if the principal is authenticated. A
/// non-authenticating authorizer returns `nullopt` from `authenticate()`, and
/// `RemoteServer` then **clears** `Context::principal` before dispatch rather
/// than passing the client's unverified claim through — so there is no
/// identity for a role check to key on at all. `polls::auth::PollsAuthorizer`
/// can derive from `AllowAllAuthorizer` precisely because it gates nothing on
/// identity; this rung cannot.
///
/// @par Where the roles come from
/// From a caller-supplied `RoleLookup`, which production wires to the
/// `lims_operators` table — the same table `SampleModel` reads. One source of
/// truth, two enforcement points. Deliberately *not* the token's own `roles`
/// claim: a token is minted once and lives for its expiry, so a role revoked
/// mid-shift would keep working until the token aged out, which in a
/// 21 CFR Part 11-framed lab is the wrong default. The claim is still
/// available and a deployment that wants token-carried roles can supply a
/// `RoleLookup` that reads it.

namespace lims::auth {

/// @brief Looks up every role a principal currently holds.
///
/// A `std::function` rather than a hard-wired database read so the policy is
/// unit-testable without a schema, and so a deployment can source roles from
/// somewhere else (LDAP, the token's own claim) without touching this class.
using RoleLookup = std::function<std::vector<LimsRole>(std::string_view principal)>;

/// @brief The actions that require `LimsRole::Verifier`.
/// @param actionType The action type id from the dispatch envelope.
/// @return `true` when the action is role-gated.
[[nodiscard]] inline bool requiresVerifierRole(std::string_view actionType) noexcept {
    return actionType == "VerifyResult";
}

/// @brief The actions that require `LimsRole::Supervisor`.
/// @param actionType The action type id from the dispatch envelope.
/// @return `true` when the action is role-gated.
[[nodiscard]] inline bool requiresSupervisorRole(std::string_view actionType) noexcept {
    return actionType == "GrantRole";
}

/// @brief This rung's authorizer: a signed-token gate plus a role check on the
///        two actions that need one.
class LimsAuthorizer : public ::morph::session::SigningAuthorizer {
public:
    /// @param secret Shared secret tokens are signed with.
    /// @param roles How to look up a principal's current roles.
    ///
    /// Names the MAC primitive explicitly rather than taking
    /// `SigningAuthorizer`'s default. Under `-DMORPH_REQUIRE_VETTED_HMAC` that
    /// default does not exist -- the option's whole purpose is to make an
    /// application call site state its choice instead of inheriting one -- and
    /// this class is exactly such a call site, so relying on the default made
    /// the rung unbuildable under that option. Passing `hmacSha256` here
    /// compiles either way and says which primitive the rung uses.
    ///
    /// A real deployment injects a vetted implementation through the overload
    /// below; see `docs/spec/security.md`, "Recommended production wiring: a vetted library".
    LimsAuthorizer(std::string secret, RoleLookup roles)
        : ::morph::session::SigningAuthorizer{std::move(secret), ::morph::session::hmacSha256},
          _roles{std::move(roles)} {}

    /// @brief As above, with an injected MAC primitive.
    /// @param secret Shared secret tokens are signed with.
    /// @param mac    MAC primitive to sign and verify with.
    /// @param roles  How to look up a principal's current roles.
    LimsAuthorizer(std::string secret, ::morph::session::MacFunction mac, RoleLookup roles)
        : ::morph::session::SigningAuthorizer{std::move(secret), std::move(mac)}, _roles{std::move(roles)} {}

    /// @brief Allows the call iff the token verifies **and** the caller holds
    ///        whatever role the action requires.
    ///
    /// The bootstrap carve-out `SampleModel::execute(GrantRole)` implements —
    /// the first grant on a lab with no supervisor — is deliberately **not**
    /// mirrored here. This layer cannot see whether the lab has a supervisor
    /// yet without a second lookup, and duplicating a security exception in two
    /// places is how the two copies drift; the model's own check is the one
    /// that decides, and it runs on every path including this one. The
    /// practical consequence is stated in the rung README: the very first
    /// `GrantRole` must be made locally (or by a principal already holding
    /// `Supervisor`), never remotely against an empty lab.
    /// @param ctx        Per-call session (its `token` is verified upstream).
    /// @param modelType  Target model type id.
    /// @param actionType Target action type id.
    /// @return `true` to allow dispatch, `false` to reject.
    [[nodiscard]] bool authorize(const ::morph::session::Context& ctx,
                                 // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
                                 std::string_view modelType, std::string_view actionType) const override {
        if (!::morph::session::SigningAuthorizer::authorize(ctx, modelType, actionType)) {
            return false;
        }
        // The token verified, so `authenticate()` gives the identity the server
        // is about to make authoritative. Keying the role check on anything
        // else — `ctx.principal` as the client sent it, say — would be keying
        // it on unverified wire input.
        const auto principal = authenticate(ctx);
        if (!principal.has_value()) {
            return false;
        }
        if (requiresVerifierRole(actionType)) {
            return holds(*principal, LimsRole::Verifier);
        }
        if (requiresSupervisorRole(actionType)) {
            return holds(*principal, LimsRole::Supervisor);
        }
        return true;
    }

private:
    /// @brief Whether @p principal currently holds @p role.
    /// @param principal The authenticated principal.
    /// @param role The role to check for.
    /// @return `true` when the lookup reports the role.
    [[nodiscard]] bool holds(const std::string& principal, LimsRole role) const {
        if (!_roles) {
            // Fail closed. An authorizer with no way to look up roles must
            // refuse the role-gated actions, not wave them through.
            return false;
        }
        const auto held = _roles(principal);
        return std::ranges::find(held, role) != held.end();
    }

    RoleLookup _roles;
};

}  // namespace lims::auth

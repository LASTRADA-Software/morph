// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/session/session_auth.hpp>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

/// @file
/// The one `IAuthorizer` every model-bearing `RemoteServer` in this rung
/// installs. Real signed-token authentication (README "Sessions &
/// authorization" -- bookmarks is the first rung to wire this end-to-end,
/// not merely touch `IAuthorizer`), plus the two hooks
/// `SigningAuthorizer` leaves at their allow-all defaults:
/// `authorizeRegister` and `authorizeInstance`.
///
/// `register`/`attach`/`assign`/`deregister` envelopes now carry the
/// client's authenticated session, so `RemoteServer` records a real,
/// non-empty owner principal for a plain-registered instance and both hooks
/// below can key on identity for it (shared/keyed instances remain recorded
/// ownerless, by a separate, deliberate design choice unrelated to session
/// plumbing — see `authorizeInstance`'s own doc comment). This rung's
/// `authorizeRegister` stays unconditionally permissive anyway (see its own
/// doc comment for why), so what is genuinely enforced -- and it *is* the
/// whole trust boundary this rung claims -- is: `SigningAuthorizer::
/// authorize()` verifying a real signed token on **every `execute`**,
/// `RemoteServer` overwriting `Context::principal` with the verified
/// identity before the model runs, and each model re-reading
/// `session::current()->principal` and scoping its own queries to it
/// (`examples/IMPLEMENTATION.md` rule 1: "models must re-check their own
/// preconditions and authorization"). An unauthenticated caller can create a
/// model instance, and nothing else: every action it could dispatch on that
/// instance is rejected by `authorize()` before a model ever sees it. The
/// resulting unauthenticated-instance-churn surface is bounded by
/// `RemoteServer::setLimitPolicy`'s `maxLiveModels`, which
/// `bookmarks::app::App` sets for exactly this reason.

namespace bookmarks::auth {

/// @brief Service principal the internal metadata-fetch worker (Task 12)
///        authenticates as. Reserved by convention, not by any framework
///        mechanism -- nothing stops a real user from registering under this
///        name too, since usernames are not a secret; the worker is
///        distinguished by holding a token only the server process itself
///        can mint (it shares the server's `TokenIssuer` secret), not by the
///        string alone.
inline constexpr std::string_view kMetadataFetcherPrincipal = "system:metadata-fetcher";

/// @brief Namespace prefix reserved for service principals such as
///        `kMetadataFetcherPrincipal`. No human may log in under it — see
///        `isReservedPrincipal`.
inline constexpr std::string_view kServicePrincipalPrefix = "system:";

/// @brief Longest principal this rung accepts, in bytes.
inline constexpr std::size_t kMaxPrincipalBytes = 64;

/// @brief Whether @p principal is acceptable as a login/registration
///        identity for this rung.
///
/// Defense-in-depth, kept even though the gap it originally guarded against
/// is now closed framework-side: `morph::session::TokenIssuer::issue()`
/// writes `SessionToken::principal` via `glz::write_json` with
/// `escape_control_characters = true` (`session_auth.hpp`), so a principal
/// containing a raw control byte no longer corrupts the token's JSON payload
/// on the way in. This validator still rejects such input at this rung's own
/// boundary regardless -- a second, independent line of defense costs
/// nothing to keep. The bound is deliberately ASCII-only and
/// short: this is a *username*, not free text, so `[A-Za-z0-9._:-]` covers
/// every reasonable login identity without needing Unicode normalization
/// decisions (contrast tag names, Task 6, which are free text and do need
/// one). `:` is included specifically so `kMetadataFetcherPrincipal`
/// (`"system:metadata-fetcher"`) itself passes this check -- the
/// `system:`-prefix service-principal convention needs a separator between
/// the namespace and the name, and `:` is the one the README already uses.
/// @param principal Candidate principal string.
/// @return `true` if @p principal is non-empty, at most `kMaxPrincipalBytes`
///         long, and every byte is an ASCII letter, digit, `.`, `_`, `:`, or `-`.
[[nodiscard]] inline bool isValidPrincipal(std::string_view principal) noexcept {
    if (principal.empty() || principal.size() > kMaxPrincipalBytes) {
        return false;
    }
    for (const char ch : principal) {
        const auto byte = static_cast<unsigned char>(ch);
        const bool ok = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
                       (byte >= '0' && byte <= '9') || byte == '.' || byte == '_' || byte == '-' ||
                       byte == ':';
        if (!ok) {
            return false;
        }
    }
    return true;
}

/// @brief Whether @p principal is reserved for the server's own internal
///        workers and must never be handed to a caller.
///
/// `kMetadataFetcherPrincipal`'s own doc comment notes that the service
/// principal is distinguished by "holding a token only the server process
/// itself can mint", not by the string. That is only true if the server
/// refuses to mint one on request — and `AuthModel::execute(const Login&)`
/// (Task 12) mints a token for whatever username it is given, since this
/// rung has no credential store. Without this check any client could log in
/// as `"system:metadata-fetcher"` and obtain a genuinely-signed service
/// token, which `BookmarkModel::execute(const RecordMetadata&)` accepts —
/// letting it rewrite the title and favicon of every other user's bookmarks.
/// The whole `system:` namespace is reserved rather than just the one known
/// name, so a later worker principal needs no change here.
/// @param principal Candidate principal string.
/// @return `true` if @p principal begins with `kServicePrincipalPrefix`.
[[nodiscard]] inline bool isReservedPrincipal(std::string_view principal) noexcept {
    return principal.starts_with(kServicePrincipalPrefix);
}

/// @brief This rung's `IAuthorizer`: real signed-token auth
///        (`SigningAuthorizer`'s inherited `authorize`/`authenticate`), plus
///        overrides of the two instance-lifecycle hooks — `authorizeRegister`
///        stays permissive by choice, `authorizeInstance` is genuinely
///        enforcing for plain-registered instances; see each hook's own doc
///        comment.
class BookmarksAuthorizer : public ::morph::session::SigningAuthorizer {
  public:
    using SigningAuthorizer::SigningAuthorizer;

    /// @brief Model type id of the one model a tokenless caller may execute on.
    static constexpr std::string_view kAnonymousModelType = "AuthModel";
    /// @brief Action type id of the one action a tokenless caller may execute.
    static constexpr std::string_view kAnonymousActionType = "Login";

    /// @brief `SigningAuthorizer::authorize`, with exactly one carve-out:
    ///        `AuthModel`/`Login` is admitted without a token.
    ///
    /// Without this the rung has a chicken-and-egg deadlock that no client can
    /// break: `SigningAuthorizer::authorize()` verifies `Context::token` on
    /// **every** `execute` and returns `false` when there is none — including
    /// for `Login`, which is the only way to obtain a token in the first
    /// place. Every action a fresh client can send is therefore answered
    /// `err "unauthorized"`, login included. This was found by driving the
    /// desktop client against a real `ladder_bookmarks_server` (task 18); the
    /// existing `Login` tests all call `AuthModel::execute()` directly, which
    /// never consults an authorizer, so nothing had exercised the login action
    /// *over a server* before.
    ///
    /// The carve-out is deliberately as narrow as it can be — one model type,
    /// one action type, both compared exactly — and it gives away nothing that
    /// was not already reachable: `AuthModel` is stateless, holds no database,
    /// and `execute(const Login&)`'s own body rejects an invalid principal and
    /// refuses the reserved `system:` namespace outright. What an anonymous
    /// caller can do here is mint a token for a username it names, which is
    /// exactly what a dev-mode login *is* (`bookmarks/dto/auth_dto.hpp`'s
    /// `@file` comment states the whole security posture plainly). Every other
    /// model and every other action still requires a validly signed, unexpired
    /// token, and `RemoteServer` still clears the client-asserted principal
    /// whenever `authenticate()` cannot vouch for it — so a `Login` dispatched
    /// anonymously runs with an *empty* `session::current()->principal`, which
    /// `AuthModel` neither reads nor needs.
    ///
    /// A real deployment replaces the body of `AuthModel::execute(const
    /// Login&)` with password/OAuth verification; the fact that its login
    /// action is reachable without a bearer token does not change, because
    /// that is what "log in" means.
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

    /// @brief Admits every registration of a type this server actually
    ///        serves, regardless of caller identity — a deliberate choice,
    ///        not a framework limitation.
    ///
    /// This was originally written as "only an authenticated caller may
    /// create an instance", copying the shape the framework's own suite
    /// documents (`tests/test_register_authorization.cpp`'s
    /// `AuthenticatedOnlyRegisterAuthorizer`), back when `register`
    /// envelopes carried no session at all and @p ctx was therefore always
    /// empty here — including for a client holding a perfectly valid token,
    /// and including the `AuthModel` handler exempted below, so that rule
    /// rejected every client's very first `BridgeHandler` construction.
    /// `register`/`attach`/`assign`/`deregister` envelopes now carry the
    /// caller's authenticated session, so @p ctx is populated when the
    /// caller holds one — but this hook stays unconditionally permissive
    /// anyway, since gating registration by identity buys nothing extra:
    /// every subsequent `execute` on the instance still goes through the
    /// inherited `SigningAuthorizer::authorize()`, which requires a validly
    /// signed, unexpired token, and then through the model's own
    /// `session::current()->principal` scoping. The `modelType` parameter
    /// stays in the signature (and the `"AuthModel"` mention stays in this
    /// comment) because the *type*-keyed half of this hook — refusing a
    /// model type outright — remains available if this rung ever needs it;
    /// the identity-keyed half is a choice not to gate, not an inability to.
    /// @param ctx       Per-call session. Populated with the caller's
    ///                  verified principal when it holds a valid token,
    ///                  empty otherwise; ignored either way (see above).
    /// @param modelType Target model type id. `RemoteServer` has already
    ///                  rejected a type its registry does not know by the
    ///                  time this runs, so every value reaching here is one
    ///                  this rung serves.
    /// @return `true`, always — see this function's own doc comment.
    [[nodiscard]] bool authorizeRegister([[maybe_unused]] const ::morph::session::Context& ctx,
                                        [[maybe_unused]] std::string_view modelType) const override {
        return true;
    }

    /// @brief Real ownership for a plain-registered instance; a pass-through
    ///        for an ownerless (shared) one.
    ///
    /// `ownerPrincipal` is the value `RemoteServer` recorded at `register`
    /// time. See `tests/test_policy_hardening.cpp`'s `OwnershipAuthorizer`
    /// for the identical one-line shape this mirrors.
    ///
    /// Genuinely enforcing today, for every plain-registered `BookmarkModel`/
    /// `TagModel`/`AuthModel` instance: `register` envelopes now carry the
    /// caller's authenticated session, so `RemoteServer` records that
    /// caller's real principal as the instance's owner, and this function
    /// denies a different principal's `execute`/`deregister` naming that
    /// instance's `modelId` directly. `SharedFeedModel` (this rung's only
    /// shared instance) still falls through the `ownerPrincipal.empty()`
    /// branch — shared instances are recorded ownerless by separate,
    /// deliberate design (there is no single owning user for a cross-user
    /// feed), not because ownership can't be tracked.
    ///
    /// What this does *not* catch, and cannot: `BridgeHandler<Model>` (this
    /// rung's only shipped client) never names another connection's
    /// `modelId` — each client only ever dispatches through its own
    /// registered instance — so a normal client's cross-user `GetBookmark{id}`
    /// (naming *another user's row* through the caller's *own* instance) is
    /// invisible to this instance-level check entirely; it would pass
    /// regardless, since it never touches an instance this caller doesn't
    /// own. That case is caught only by `BookmarkModel::execute`'s own
    /// row-level re-check (see `tests/test_bookmark_model.cpp`'s "denied by
    /// the model's own ownership re-check" case), which is the *only* layer
    /// that could ever catch it — a per-instance check has no way to express
    /// a per-row constraint. This function's real target is a client that
    /// does not go through `BridgeHandler` at all: a raw wire client crafting
    /// an `execute`/`deregister` envelope naming a `modelId` it learned or
    /// guessed, belonging to an instance it never registered.
    /// @param ctx            Per-call session; `principal` is the verified identity.
    /// @param modelType      Ignored: the same rule applies to every model.
    /// @param actionType     Ignored.
    /// @param modelId        Ignored: the decision only needs the owner.
    /// @param ownerPrincipal Principal recorded as the instance's owner, or
    ///                       empty if none was recorded (a shared instance).
    /// @return `true` if @p ownerPrincipal is empty or matches `ctx.principal`.
    [[nodiscard]] bool authorizeInstance(const ::morph::session::Context& ctx,
                                        [[maybe_unused]] std::string_view modelType,
                                        [[maybe_unused]] std::string_view actionType,
                                        [[maybe_unused]] std::uint64_t modelId,
                                        std::string_view ownerPrincipal) const override {
        return ownerPrincipal.empty() || ownerPrincipal == ctx.principal;
    }
};

namespace detail {

/// @brief Backing storage for `setTokenIssuer`/`tokenIssuer` — a single
///        shared slot, guarded by a single mutex. Not exposed directly;
///        both public functions below go through this pair, so they
///        genuinely observe each other's writes (unlike two independent
///        function-local statics, which would each own an unrelated slot).
[[nodiscard]] inline std::mutex& tokenIssuerMutex() {
    static std::mutex mtx;
    return mtx;
}

[[nodiscard]] inline std::shared_ptr<::morph::session::TokenIssuer>& tokenIssuerSlot() {
    static std::shared_ptr<::morph::session::TokenIssuer> slot;
    return slot;
}

}  // namespace detail

/// @brief Installs @p issuer as the process-global `TokenIssuer`, mirroring
///        `morph::journal::setActionLog`'s identical shape — the same answer
///        `AuthModel` (Task 12) reaches for since it is registered via the
///        plain `BRIDGE_REGISTER_MODEL` default-construction path rather
///        than `ModelRegistryFactory`'s per-instance construction-hook seam
///        (`include/morph/core/registry.hpp`): a process-global slot passes
///        the secret through instead. `App` calls this once at startup,
///        with the *same* secret it hands to `BookmarksAuthorizer`, so a
///        token `AuthModel::execute(const Login&)` mints verifies against
///        the very authorizer that will check every subsequent call.
/// @param issuer The issuer every `AuthModel` instance will read, or
///        `nullptr` to clear it (tests do this via `DbFixture`-adjacent
///        RAII if a test needs isolation — see `test_app.cpp`'s login case,
///        Task 12).
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

}  // namespace bookmarks::auth

// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace morph::session {

/// @brief Per-call session context carried through the bridge to the model.
///
/// `Context` is an open data bag: callers populate whichever fields make sense
/// for the app (user identity, request tracing, locale, app-specific metadata).
/// The framework only inspects `principal` and the action ids when consulting
/// the configured `IAuthorizer`; everything else is passed through verbatim
/// from caller to model.
///
/// On remote backends the `Context` is serialised into the wire envelope, so
/// the server-side `RemoteServer` sees the same values the GUI sent. On the
/// local backend it travels in-memory via the `ActionCall`.
struct Context {
    /// @brief Auth principal (user id).
    ///
    /// On the client this is whatever the caller sets and is **not** trustworthy
    /// on its own — it is untrusted wire input. When a verifying authorizer (e.g.
    /// `SigningAuthorizer`, `session_auth.hpp`) is installed, `RemoteServer`
    /// **overwrites** this field with the principal extracted from a valid
    /// `token` before dispatch, so `session::current()->principal` read inside a
    /// model is the authenticated identity, not the client's claim.
    std::string principal;

    /// @brief Bearer credential verified server-side (empty if unauthenticated).
    ///
    /// Typically a signed token minted by a login action via
    /// `session::TokenIssuer` and attached to every call (see `session_auth.hpp`
    /// and `docs/spec/security.md`). Travels in the wire envelope's `session`.
    std::string token;

    /// @brief Stable id used for distributed tracing / log correlation. Empty if unused.
    std::string requestId;

    /// @brief BCP-47 locale tag (e.g. `en-US`, `fr-FR`) for i18n. Empty for app default.
    std::string locale;

    /// @brief Free-form bag of string→string metadata (feature flags, A/B buckets, …).
    std::unordered_map<std::string, std::string> metadata;
};

/// @brief The signed-in user's verified identity, readable outside a dispatch.
///
/// `Context::principal` only exists *during* a dispatch — `session::current()`
/// returns `nullptr` outside one, so UI code (a button's enabled state, a menu
/// item's visibility) has no way to ask "who is signed in and what may they do?"
/// without attempting the action and catching the failure. `Principal` is the
/// longer-lived counterpart an application installs once, typically right
/// after a successful login dispatch, and reads from UI code to shape itself
/// instead: `bridge.currentPrincipal().hasRole("editor")`.
///
/// Deliberately **not** a process-wide global: the caller installs it on the
/// specific `Bridge` (or other session-scoped object) whose backend it came
/// from — see `Bridge::setPrincipal`/`Bridge::currentPrincipal`
/// (`core/bridge.hpp`) — rather than one ambient value shared by every backend
/// a process happens to hold.
///
/// @warning Populate this only from data the server actually returned (e.g. an
/// action's `Result`, such as the bank example's `AuthResult`), never from a
/// client-side guess — the server remains the sole authority for what a
/// principal may do; this is a read-only cache of what it last told the
/// client, not a second, independently-decided permission set. Roles can
/// change server-side mid-session, so every dispatch is still authorized
/// there regardless of what a stale `Principal` says client-side; this only
/// shapes the UI, it never replaces server-side authorization.
struct Principal {
    /// @brief Verified identity (e.g. username), as returned by the server. Empty if signed out.
    std::string id;

    /// @brief Coarse-grained roles the app can gate UI on (e.g. `"editor"`, `"admin"`).
    std::vector<std::string> roles;

    /// @brief Free-form bag of string→string claims beyond `id`/`roles`.
    std::unordered_map<std::string, std::string> claims;

    /// @brief Returns `true` if `roles` contains @p role.
    /// @param role Role name to look for.
    /// @return `true` if @p role is present in `roles`.
    [[nodiscard]] bool hasRole(std::string_view role) const { return std::ranges::find(roles, role) != roles.end(); }
};

/// @brief Authorizes incoming actions on a `RemoteServer`.
///
/// Called once per `execute` envelope, before the action is dispatched. A `false`
/// return causes the server to reply with `err|unauthorized` (the client surfaces
/// the error through the `.onError(...)` callback).
///
/// Default implementation supplied by the framework is `AllowAllAuthorizer`. Real
/// deployments install a custom subclass that checks principal claims, action
/// permissions, rate limits, etc.
// NOLINTBEGIN(cppcoreguidelines-special-member-functions)
struct IAuthorizer {
    virtual ~IAuthorizer() = default;

    /// @brief Returns `true` if @p ctx is allowed to invoke @p actionType on @p modelType.
    ///
    /// @param ctx        Per-call session attached by the client.
    /// @param modelType  String id of the target model type.
    /// @param actionType String id of the action being invoked.
    /// @return `true` to allow dispatch, `false` to reject with `err|unauthorized`.
    [[nodiscard]] virtual bool authorize(const Context& ctx,
                                         // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
                                         std::string_view modelType, std::string_view actionType) const = 0;

    /// @brief Returns the authenticated principal for @p ctx, or `nullopt`.
    ///
    /// Called by `RemoteServer` after `authorize` succeeds. If it returns a
    /// value, the server overwrites `Context::principal` with it before dispatch,
    /// making the identity authoritative for model code that reads
    /// `session::current()`. The default returns `nullopt` — for authorizers that
    /// do not authenticate (e.g. `AllowAllAuthorizer`), `RemoteServer` **clears**
    /// `Context::principal` to the empty string before dispatch rather than
    /// passing the client's unverified claim through, so model code never sees an
    /// unauthenticated principal presented as authoritative. A verifying authorizer
    /// (`SigningAuthorizer`) overrides this to supply the token-derived identity.
    /// @param ctx Per-call session attached by the client.
    /// @return The verified principal to make authoritative, or `nullopt`.
    [[nodiscard]] virtual std::optional<std::string> authenticate([[maybe_unused]] const Context& ctx) const {
        return std::nullopt;
    }

    /// @brief Optional per-**instance** authorization hook — the multi-tenant gate.
    ///
    /// `authorize` sees only the model *type*, so it cannot answer "may this
    /// caller touch *this instance* (row)?". Model instances on a `RemoteServer`
    /// are addressable by guessable sequential ids, so without an ownership check
    /// any authenticated caller can `execute`/`deregister` against an id it did
    /// not create — a cross-tenant targeting gap. This hook closes it: when
    /// installed, `RemoteServer` consults it on every `execute` **and** every
    /// `deregister`, passing the id of the target instance and the principal
    /// recorded as its owner at `register` time (empty if the instance was
    /// registered by an unauthenticated caller or predates ownership tracking).
    ///
    /// The **default allows everything**, so an authorizer that does not override
    /// it — including `AllowAllAuthorizer` and a plain `SigningAuthorizer` — keeps
    /// the pre-existing behaviour exactly (no per-instance restriction). A
    /// deployer opts into ownership enforcement by overriding this, typically to
    /// compare @p ownerPrincipal against `ctx.principal`:
    /// @code
    /// bool authorizeInstance(const Context& ctx, std::string_view, std::string_view,
    ///                        uint64_t, std::string_view ownerPrincipal) const override {
    ///     return ownerPrincipal.empty() || ownerPrincipal == ctx.principal;
    /// }
    /// @endcode
    ///
    /// @param ctx            Per-call session (its `principal` is authoritative
    ///                       only if a verifying authorizer is installed).
    /// @param modelType      Target model type id (empty for `deregister`).
    /// @param actionType     Target action type id (empty for `deregister`).
    /// @param modelId        Numeric id of the target instance.
    /// @param ownerPrincipal Principal recorded as the instance's owner at
    ///                       `register` time; empty if none was recorded.
    /// @return `true` to allow the operation on this instance, `false` to reject.
    [[nodiscard]] virtual bool authorizeInstance([[maybe_unused]] const Context& ctx,
                                                 [[maybe_unused]] std::string_view modelType,
                                                 [[maybe_unused]] std::string_view actionType,
                                                 [[maybe_unused]] std::uint64_t modelId,
                                                 [[maybe_unused]] std::string_view ownerPrincipal) const {
        return true;
    }

    /// @brief Optional gate on model **creation** — bounds *who may create* an instance.
    ///
    /// `authorize` and `authorizeInstance` both act on an already-existing
    /// instance; neither can answer "may this caller create one at all?".
    /// `RemoteServer` consults this hook on every `register` envelope, after
    /// authenticating the caller (so @p ctx carries the *verified* principal,
    /// not the client's raw claim) and before constructing the instance. A
    /// `false` return replies `err "unauthorized"` and **no instance is
    /// created** — `ModelRegistryFactory::create` never runs.
    ///
    /// The **default allows everything**, so an authorizer that does not
    /// override it — including `AllowAllAuthorizer` and a plain
    /// `SigningAuthorizer` — keeps the pre-existing behaviour exactly (any
    /// reachable client may register any known model type). A deployer opts
    /// into bounding registration by overriding this, typically requiring
    /// authentication and/or restricting @p modelType:
    /// @code
    /// bool authorizeRegister(const Context& ctx, std::string_view modelType) const override {
    ///     return !ctx.principal.empty();  // only authenticated callers may register
    /// }
    /// @endcode
    ///
    /// @param ctx       Per-call session for the register envelope; its
    ///                  `principal` is already the verified identity when the
    ///                  authorizer authenticates (empty otherwise).
    /// @param modelType String id of the model type being registered.
    /// @return `true` to allow the registration, `false` to reject with
    ///         `err "unauthorized"`.
    [[nodiscard]] virtual bool authorizeRegister([[maybe_unused]] const Context& ctx,
                                                 [[maybe_unused]] std::string_view modelType) const {
        return true;
    }
};
// NOLINTEND(cppcoreguidelines-special-member-functions)

/// @brief Default authorizer that permits everything.
///
/// Wire it explicitly via `RemoteServer(pool, dispatcher, registry, allowAll)`
/// for documentation, or rely on the server's default (which uses this type).
struct AllowAllAuthorizer : IAuthorizer {
    /// @brief Permits every call.
    /// @param ctx        Ignored.
    /// @param modelType  Ignored.
    /// @param actionType Ignored.
    /// @return Always `true`.
    [[nodiscard]] bool authorize([[maybe_unused]] const Context& ctx,
                                 // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
                                 [[maybe_unused]] std::string_view modelType,
                                 [[maybe_unused]] std::string_view actionType) const override {
        return true;
    }
};

/// @brief Returns the process-wide allow-all authorizer (used as the default).
inline std::shared_ptr<IAuthorizer> allowAllAuthorizer() {
    static auto instance = std::make_shared<AllowAllAuthorizer>();
    return instance;
}

namespace detail {

/// @brief Thread-local pointer to the `Context` for the currently dispatched action.
///
/// Installed by the backend around the model call — `LocalBackend::execute` on the
/// local path and `RemoteServer::dispatchExecute` on the remote path — via a
/// `ScopedContext` that clears it again on scope exit. `ActionDispatcher::dispatch`
/// itself does not touch this pointer. Models that opt in to session-aware logic
/// call `current()` to read it.
inline const Context*& tlsCurrent() {
    thread_local const Context* tls = nullptr;
    return tls;
}

/// @brief RAII helper that sets the thread-local `Context` for its scope.
class ScopedContext {
public:
    /// @brief Installs @p ctx as the thread-local context until the scope exits.
    /// @param ctx Context whose address is stored; must outlive this object.
    explicit ScopedContext(const Context& ctx) : _prev{tlsCurrent()} { tlsCurrent() = &ctx; }
    /// @brief Restores the previously active thread-local context.
    ~ScopedContext() { tlsCurrent() = _prev; }
    ScopedContext(const ScopedContext&) = delete;
    ScopedContext& operator=(const ScopedContext&) = delete;
    ScopedContext(ScopedContext&&) = delete;
    ScopedContext& operator=(ScopedContext&&) = delete;

private:
    const Context* _prev;
};

}  // namespace detail

/// @brief Returns the active `Context` for the in-progress action, or `nullptr` if none.
///
/// Models that don't need session data can ignore this entirely. Models that do
/// can read principal/locale/metadata without changing their `execute()` signature.
inline const Context* current() noexcept { return detail::tlsCurrent(); }

}  // namespace morph::session

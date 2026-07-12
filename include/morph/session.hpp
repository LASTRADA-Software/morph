// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

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
    /// @brief Auth principal (user id, JWT, session token — application-defined).
    std::string principal;

    /// @brief Stable id used for distributed tracing / log correlation. Empty if unused.
    std::string requestId;

    /// @brief BCP-47 locale tag (e.g. `en-US`, `fr-FR`) for i18n. Empty for app default.
    std::string locale;

    /// @brief Free-form bag of string→string metadata (feature flags, A/B buckets, …).
    std::unordered_map<std::string, std::string> metadata;
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
                                         std::string_view modelType,
                                          std::string_view actionType) const = 0;
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
/// Set by `ActionDispatcher::dispatch` while the model's `execute()` runs, then
/// cleared. Models that opt in to session-aware logic call `current()` to read it.
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
inline const Context* current() noexcept {
    return detail::tlsCurrent();
}

}  // namespace morph::session

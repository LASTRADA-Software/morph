// SPDX-License-Identifier: Apache-2.0
#include "kanban/app/app.hpp"

// Every model this server hosts is included here, not only the ones this
// file references by name. `BRIDGE_REGISTER_MODEL`/`BRIDGE_REGISTER_ACTION`
// place their registrars in the *header*, so a translation unit that
// includes the header both registers the type with the process-wide
// registry/dispatcher and emits a reference to that model's `execute`
// bodies -- which is what pulls each model's object file out of the static
// library for a binary (a server `main()`) whose own code names nothing but
// `App`. Without this, such a binary would either fail to link or come up
// serving no models at all. Mirrors `bookmarks::app::app.cpp`'s identical
// comment and reasoning.
#include <morph/session/session_auth.hpp>
#include <string_view>
#include <utility>

#include "kanban/models/board_model.hpp"
#include "kanban/models/project_admin_model.hpp"

namespace kanban::app {

namespace {

/// @brief Live-instance cap this server installs.
///
/// Mirrors `bookmarks::app::App`'s own `kMaxLiveModels` treatment: this
/// rung's `KanbanAuthorizer::authorizeRegister` is permissive by default
/// (`kanban/auth/kanban_authorizer.hpp`'s own `@file` comment), so an
/// unauthenticated client can still make the server create model instances
/// even though it can never execute anything useful on them once
/// `SigningAuthorizer::authorize` rejects the call. `maxLiveModels` is the
/// framework's own answer to that shape of churn. The value is generous —
/// no real deployment at this rung's scale approaches it — chosen only to
/// bound unauthenticated registration churn, not to constrain legitimate use.
constexpr std::size_t kMaxLiveModels = 256;

}  // namespace

App::App(std::filesystem::path actionLogPath, std::string tokenSecret, std::size_t workers)
    : _actionLog{std::make_shared<::morph::journal::FileActionLog>(std::move(actionLogPath))},
      _pool{workers},
      // hmacSha256 named explicitly -- same reason as bookmarks::App's own
      // two TokenIssuer/authorizer call sites: SigningAuthorizer/TokenIssuer
      // both inherit a MacFunction default that MORPH_REQUIRE_VETTED_HMAC
      // drops entirely (see TokenIssuer's own doc comment), so this call
      // site must keep compiling under that build option with the identical
      // MAC it always used.
      _server{std::make_shared<::morph::backend::RemoteServer>(
          _pool, std::make_shared<auth::KanbanAuthorizer>(tokenSecret, ::morph::session::hmacSha256))} {
    // Installed process-wide so any *default-constructed* (non-keyed,
    // unshared) model -- ProjectAdminModel, AuthModel, and a BoardModel
    // registered via the plain (non-shared) path -- auto-attaches via
    // `ModelFactory::create<Model>()` (`morph/core/model.hpp`). Sufficient
    // for those models: none of them keeps a model-level `IActionLog`
    // member the way `BoardModel` does.
    ::morph::journal::setActionLog(_actionLog);

    // Installed process-wide so AuthModel::execute(const Login&) can mint
    // tokens against this exact secret -- the same "registry-constructed
    // models are always default-constructed, so there is no DI seam" answer
    // morph::journal::setActionLog already uses one line above. hmacSha256
    // named explicitly for the same MORPH_REQUIRE_VETTED_HMAC reason as the
    // authorizer above -- and so both issuers stay verifiably the same MAC,
    // which they must be: the authorizer this rung installs verifies every
    // token against whichever MAC minted it.
    auth::setTokenIssuer(std::make_shared<::morph::session::TokenIssuer>(tokenSecret, ::morph::session::hmacSha256));

    // `RemoteServer::LogProvider` is the second, *necessary* action-log
    // attach path -- see docs/spec/journal/journal.md, "Attaching a log to
    // remote instances", and morph::model::detail::IModelHolder::
    // onActionLogAttached's own doc comment (morph/core/model.hpp). A
    // *keyed/shared* BoardModel instance is registered via `RemoteServer`'s
    // `register`/`attach` envelope path (`acquireSharedInstance`,
    // morph/core/remote.hpp), which calls `_registry.create(env.typeId)`
    // (the plain default-construction factory `BRIDGE_REGISTER_MODEL`
    // installs, run *before* `morph::journal::setActionLog` above has any
    // bearing on this particular instance) and *then*
    // `attachLogIfConfigured(*holder, env)` -- `env.contextKey` (== the
    // project id string, since `BridgeHandler<BoardModel,
    // AllowShared>::attachHandler` sets `contextKey = primary`,
    // `morph/core/bridge.hpp`) is only known at that later point, not at
    // holder-construction time, so the process-wide default log
    // `ModelFactory::create` reads is not the mechanism that reaches this
    // path at all. `setLogProvider` is what lets this App supply *this
    // exact* `_actionLog` instance for that later attach. Once supplied,
    // `IModelHolder::attachActionLog` (called from `attachLogIfConfigured`)
    // forwards to `ModelHolder<BoardModel>::onActionLogAttached`, which
    // structurally detects `BoardModel::attachActionLog`
    // (`ModelLevelActionLogAttachable`, morph/core/model.hpp) and calls it --
    // so `BoardModel::_log` ends up holding the *same* `IActionLog` instance
    // the holder's own `recordIfAttached` auto-append writes to, not a
    // separate log, not no log. `GetActivity` reads it back.
    //
    // The same provider is installed for every model type (the callback
    // ignores `modelType`) rather than gated to "BoardModel" by name: in
    // practice only a `register` envelope carrying a non-empty `contextKey`
    // ever consults it at all, and only `BoardModel`'s `AllowShared`
    // keyed-attach path sets one -- `ProjectAdminModel`/`AuthModel` are
    // registered plain (no `contextKey`), so `attachLogIfConfigured` never
    // calls this provider for them (see that method's own early-return on
    // an empty `contextKey`, morph/core/remote.hpp).
    _server->setLogProvider([log = _actionLog](std::string_view /*modelType*/, std::string_view /*contextKey*/)
                                -> std::shared_ptr<::morph::journal::IActionLog> { return log; });

    ::morph::backend::LimitPolicy limits;
    limits.maxLiveModels = kMaxLiveModels;
    _server->setLimitPolicy(limits);
}

App::~App() {
    ::morph::journal::setActionLog(nullptr);
    // Matches setActionLog's own clear-on-destruction discipline: a later
    // test (or a second App in the same process) must see
    // auth::tokenIssuer() == nullptr rather than a previous App's still-live
    // issuer, which would be holding a *different* secret than whatever
    // authorizer is current.
    auth::setTokenIssuer(nullptr);
}

}  // namespace kanban::app

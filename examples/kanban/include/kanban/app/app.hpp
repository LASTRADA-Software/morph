// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "kanban/auth/kanban_authorizer.hpp"

#include <morph/core/executor.hpp>
#include <morph/core/remote.hpp>
#include <morph/journal/file_action_log.hpp>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>

/// @file
/// `kanban::app::App` -- this rung's server bootstrap. Mirrors
/// `bookmarks::app::App` (`examples/bookmarks/include/bookmarks/app/app.hpp`)
/// closely, minus everything that rung's `App` owns and this one has no
/// equivalent for:
///
///  - No background worker/timer, and therefore no `QObject`/`QTimer`
///    inheritance and no internal client `Bridge`. Every mutation this
///    rung's `BoardModel` performs (manage cards, comments, move tasks) is
///    synchronous, immediate, inside the calling `execute()` -- there is no
///    async job (no metadata fetch, no expiry sweep, no outbox relay) for a
///    timer to drive. `App` is therefore plain C++, not Qt-dependent at
///    all: only the *tests* that dispatch a real client through `server()`
///    need Qt (for `BridgeHandler`'s completion delivery), not `App`
///    itself.
namespace kanban::app {

/// @brief Owns the server-side pieces this rung's deployment shares: the
/// worker pool, the `RemoteServer` with a real `auth::KanbanAuthorizer`
/// (`SigningAuthorizer`-derived) installed, the durable `FileActionLog`
/// (installed process-wide via `morph::journal::setActionLog`, so every
/// default-constructed model auto-attaches, *and* wired as this server's
/// `RemoteServer::LogProvider` so a registry-constructed, keyed `BoardModel`
/// instance's own `attachActionLog` also sees it -- see `app.cpp`'s
/// constructor comment for why both attach paths are needed), and the
/// process-global `TokenIssuer` `AuthModel::execute(const Login&)` mints
/// tokens from (`auth::setTokenIssuer`). Nothing here decides deployment
/// mode -- that stays `examples/common/gui::AppContext`'s job on the client
/// side; this is exclusively the server side.
class App {
  public:
    /// @brief Wires up the whole server side: worker pool, `RemoteServer`
    ///        (with `auth::KanbanAuthorizer` and this rung's `maxLiveModels`
    ///        cap installed), the durable action log, and the process-global
    ///        `TokenIssuer`.
    /// @param actionLogPath Where `FileActionLog` persists entries.
    /// @param tokenSecret   Shared secret for the `auth::KanbanAuthorizer`
    ///        this server installs and for the process-global `TokenIssuer`
    ///        `AuthModel` mints user tokens from. Both must be the same
    ///        value, which is why there is one parameter: a token minted by
    ///        one has to verify against the other.
    /// @param workers       Size of the model worker pool.
    explicit App(std::filesystem::path actionLogPath, std::string tokenSecret, std::size_t workers = 4);

    /// @brief Detaches the process-wide default action log and token issuer.
    ~App();

    App(const App&) = delete;
    App& operator=(const App&) = delete;
    App(App&&) = delete;
    App& operator=(App&&) = delete;

    /// @brief The server every transport (a `QtWebSocketServer`, a test's
    ///        `SimulatedRemoteBackend`) wraps or dispatches against.
    /// @return The shared `RemoteServer`; never null.
    [[nodiscard]] std::shared_ptr<::morph::backend::RemoteServer> server() const noexcept { return _server; }

  private:
    std::shared_ptr<::morph::journal::FileActionLog> _actionLog;
    ::morph::exec::ThreadPoolExecutor _pool;
    std::shared_ptr<::morph::backend::RemoteServer> _server;
};

}  // namespace kanban::app

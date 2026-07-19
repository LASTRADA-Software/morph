// SPDX-License-Identifier: Apache-2.0

#include "bank/app/app.hpp"

#include <Lightweight/Lightweight.hpp>
#include <morph/core/backend.hpp>
#include <morph/session/session.hpp>

#include <memory>

#include "bank/db/database.hpp"
#include "bank/db/entities.hpp"
#include "bank/db/user_ops.hpp"

namespace bank::app {

App::App(const std::string& connectionString, std::size_t workers)
    : _pool{workers},
      _bridge{std::make_unique<morph::backend::LocalBackend>(_pool)} {
    db::setup(connectionString);
}

void App::login(const std::string& principal) {
    // Owned records reference their owner by `user_id`, so the principal must
    // map to a `users` row. Provision it on login as a demo convenience — this
    // is where a registered user would normally already exist (the CLI/GUI
    // register first); tests can simply `login("alice")` and start banking.
    if (!principal.empty()) {
        Lightweight::DataMapper dm;
        db::ensureUser(dm, principal);
    }
    morph::session::Context ctx;
    ctx.principal = principal;
    _bridge.setDefaultSession(std::move(ctx));
}

void App::logout() {
    _bridge.setDefaultSession({});
}

}  // namespace bank::app

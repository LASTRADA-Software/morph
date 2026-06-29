// SPDX-License-Identifier: Apache-2.0

#include "bank/app/app.hpp"

#include <morph/backend.hpp>
#include <morph/session.hpp>

#include <memory>

#include "bank/db/database.hpp"

namespace bank::app {

App::App(const std::string& connectionString, std::size_t workers)
    : _pool{workers},
      _bridge{std::make_unique<morph::backend::LocalBackend>(_pool)} {
    db::setup(connectionString);
}

void App::login(const std::string& principal) {
    morph::session::Context ctx;
    ctx.principal = principal;
    _bridge.setDefaultSession(std::move(ctx));
}

void App::logout() {
    _bridge.setDefaultSession({});
}

}  // namespace bank::app

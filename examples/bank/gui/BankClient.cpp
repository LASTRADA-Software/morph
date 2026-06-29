// SPDX-License-Identifier: Apache-2.0

#include "BankClient.hpp"

#include <morph/backend.hpp>
#include <morph/session.hpp>

#include <memory>
#include <utility>

#include "bank/db/database.hpp"

namespace bankgui {

BankClient::BankClient(const std::string& connectionString, std::size_t workers)
    : _pool{workers}, _bridge{std::make_unique<morph::backend::LocalBackend>(_pool)} {
    bank::db::setup(connectionString);
}

void BankClient::login(const QString& principal, const QString& displayName) {
    _principal = principal;
    _displayName = displayName;
    morph::session::Context ctx;
    ctx.principal = principal.toStdString();
    _bridge.setDefaultSession(std::move(ctx));
}

void BankClient::logout() {
    _principal.clear();
    _displayName.clear();
    _bridge.setDefaultSession({});
}

}  // namespace bankgui

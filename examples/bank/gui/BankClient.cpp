// SPDX-License-Identifier: Apache-2.0

#include "BankClient.hpp"

#include <morph/core/backend.hpp>
#include <morph/session/session.hpp>

#include <memory>
#include <utility>

#ifndef __EMSCRIPTEN__
#include "bank/db/database.hpp"
#endif

namespace bankgui {

#ifdef __EMSCRIPTEN__
// WebAssembly: no ODBC/SQLite. Models run on the single-threaded Qt event loop
// (`_gui`) and persist to the in-memory store (see gui_wasm/). The connection
// string and worker count are ignored.
BankClient::BankClient(const std::string& /*connectionString*/, std::size_t /*workers*/)
    : _bridge{std::make_unique<morph::backend::LocalBackend>(_gui)} {}
#else
BankClient::BankClient(const std::string& connectionString, std::size_t workers)
    : _pool{workers}, _bridge{std::make_unique<morph::backend::LocalBackend>(_pool)} {
    bank::db::setup(connectionString);
}
#endif

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

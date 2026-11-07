// SPDX-License-Identifier: Apache-2.0
//
// ladder_lims_server: serves lims' screens and models over a Qt WebSocket, behind `LimsAuthorizer`. A client signs in
// on the public login screen; the token it gets is what every later call carries, and the role-gated actions check
// the roles `GrantRole` gave its principal.
//
// Environment: LIMS_TOKEN_SECRET (required, non-empty), LIMS_PORT (default 8771), LIMS_DB (default lims.db).
// LIMS_CHAT_PROVIDER turns the screens' chat on, with LIMS_CHAT_API_KEY, LIMS_CHAT_MODEL and optionally
// LIMS_CHAT_BASE_URL; a configuration that does not work stops the server, rather than leave it running with no chat
// and no word why.

#include <QCoreApplication>
#include <QObject>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <lims_screens.hpp>
#include <memory>
#include <morph/core/executor.hpp>
#include <morph/core/remote.hpp>
#include <morph/journal/file_action_log.hpp>
#include <morph/qt/qt_chat_provider.hpp>
#include <morph/qt/qt_executor.hpp>
#include <morph/qt/qt_websocket_server.hpp>
#include <morph/session/session_auth.hpp>
#include <morph/ui/chat/service.hpp>
#include <morph/ui/service.hpp>
#include <string>
#include <string_view>

#include "lims/auth/lims_authorizer.hpp"
#include "lims/db/database.hpp"
#include "lims/models/auth_model.hpp"
#include "lims/models/ignition_model.hpp"
#include "lims/models/sample_model.hpp"

namespace {

/// @brief The value of environment variable @p name, or @p fallback when it is unset.
/// @param name     The variable.
/// @param fallback What an unset one reads as.
/// @return The value.
std::string environment(char const* name, std::string_view fallback) {
    char const* const value =
        std::getenv(name);  // NOLINT(concurrency-mt-unsafe): read once, before any thread starts.
    return value == nullptr ? std::string{fallback} : std::string{value};
}

}  // namespace

int main(int argc, char** argv) {
    try {
        QCoreApplication const app{argc, argv};
        std::string const secret = environment("LIMS_TOKEN_SECRET", "");
        if (secret.empty()) {
            std::cerr << "lims-server: LIMS_TOKEN_SECRET must be set to a non-empty value\n";
            return 1;
        }
        auto const port = static_cast<std::uint16_t>(std::stoul(environment("LIMS_PORT", "8771")));
        lims::db::setup("DRIVER=SQLite3;Database=" + environment("LIMS_DB", "lims.db"));
        // The issuer LimsLogin mints with and the authoriser's verifier share one secret, so a token minted here
        // verifies here.
        lims::auth::setTokenIssuer(std::make_shared<morph::session::TokenIssuer>(secret, morph::session::hmacSha256));
        auto bundle = lims::screens::bundle();
        if (!bundle) {
            std::cerr << "lims-server: the screens do not build: " << bundle.error().message() << '\n';
            return 1;
        }
        // Ignition tests journal through their registration; the other models journal for themselves.
        morph::qt::QtExecutor journalOwner;
        lims::attachIgnitionLog(std::make_shared<morph::journal::FileActionLog>(
            journalOwner, std::filesystem::current_path() / "lims_ignition_actions.jsonl"));
        morph::exec::ThreadPoolExecutor chatWorkers{2};
        morph::exec::ThreadPoolExecutor workers{4};
        morph::backend::ServerConfig config;
        config.services.push_back(std::make_shared<morph::ui::UiService>(*bundle));
        QObject providerOwner;
        if (std::string const name = environment("LIMS_CHAT_PROVIDER", ""); !name.empty()) {
            auto provider = morph::qt::makeQtChatProvider(
                morph::qt::ChatProviderConfig{.name = name,
                                              .baseUrl = environment("LIMS_CHAT_BASE_URL", ""),
                                              .apiKey = environment("LIMS_CHAT_API_KEY", ""),
                                              .model = environment("LIMS_CHAT_MODEL", "")},
                &providerOwner);
            if (!provider) {
                std::cerr << "lims-server: " << provider.error() << '\n';
                return 1;
            }
            morph::ui::chat::ChatOptions chat;
            chat.provider = *provider;
            chat.worker = &chatWorkers;
            chat.bundle = *bundle;
            config.services.push_back(std::make_shared<morph::ui::chat::ChatService>(std::move(chat)));
        }
        auto const server = std::make_shared<morph::backend::RemoteServer>(
            workers,
            std::make_shared<lims::auth::LimsAuthorizer>(
                secret, morph::session::hmacSha256,
                [](std::string_view principal) { return lims::SampleModel::rolesOf(principal); }),
            config);
        morph::qt::QtWebSocketServer socket{*server, port};
        if (!socket.listen()) {
            std::cerr << "lims-server: cannot listen on port " << port << '\n';
            return 1;
        }
        std::cout << "lims-server: listening on ws://127.0.0.1:" << socket.port() << '\n' << std::flush;
        int const code = QCoreApplication::exec();
        lims::auth::setTokenIssuer(nullptr);
        return code;
    } catch (std::exception const& error) {
        std::cerr << "lims-server: " << error.what() << '\n';
        return 1;
    }
}

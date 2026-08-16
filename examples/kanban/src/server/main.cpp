// SPDX-License-Identifier: Apache-2.0

/// @file
/// kanban standalone server process: `kanban::db::setup()` once, one
/// `kanban::app::App` (worker pool + `RemoteServer` with a real
/// `auth::KanbanAuthorizer` + durable action log + process-global
/// `TokenIssuer`), and one `morph::qt::QtWebSocketServer` in front of it.
/// Mirrors `bookmarks::src::server::main.cpp` closely, minus the background
/// worker to drain on shutdown (`kanban::app::App` is plain C++ with no timer
/// at all -- see that header's own `@file` comment).
///
/// Usage:
/// @code
/// KANBAN_TOKEN_SECRET=... KANBAN_DB=... KANBAN_PORT=8768 ladder_kanban_server
/// @endcode

#include "kanban/app/app.hpp"
#include "kanban/auth/kanban_authorizer.hpp"
#include "kanban/db/database.hpp"

#include <morph/qt/qt_websocket_server.hpp>
#include <morph/session/session_auth.hpp>

#include <QCoreApplication>
#include <QTimer>

#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string_view>
#include <system_error>

#include <charconv>

namespace {

/// @brief Set from the `SIGINT`/`SIGTERM` handler, polled by a `QTimer`.
///
/// A signal handler may not call into Qt (nothing in `QCoreApplication` is
/// async-signal-safe), so it does the one thing it is allowed to do — assign
/// to a `volatile std::sig_atomic_t` — and a timer on the Qt thread turns that
/// into a real `quit()`. This exists so the shutdown path below is actually
/// *reachable*: a demo server is stopped with Ctrl-C, and the default `SIGINT`
/// disposition would terminate the process outright, so `exec()` would never
/// return and `App`'s destructor would never run at all. Identical in shape to
/// `bookmarks`' and `pastebin`'s own server mains.
volatile std::sig_atomic_t gStopRequested = 0;

extern "C" void onStopSignal(int /*signum*/) { gStopRequested = 1; }

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication qtApp{argc, argv};

    for (int i = 1; i < argc; ++i) {
        std::cerr << "kanban-server: unknown argument '" << argv[i] << "' (usage: KANBAN_TOKEN_SECRET=... ladder_kanban_server)\n";
        return 2;
    }

    // Required, with no default: the secret signs every token this server
    // mints and verifies every token it is shown, so a built-in fallback
    // would be a published signing key. Refusing to start is the only honest
    // behavior (`docs/spec/security.md`).
    const char* tokenSecretEnv = std::getenv("KANBAN_TOKEN_SECRET");
    if (tokenSecretEnv == nullptr || *tokenSecretEnv == '\0') {
        std::cerr << "kanban-server: KANBAN_TOKEN_SECRET must be set to a non-empty value\n";
        return 2;
    }
    const std::string tokenSecret{tokenSecretEnv};
    // Cleared from the environment the moment it has been copied. The
    // environment block is readable for the process's whole lifetime — by
    // anything that later calls `getenv`, by a crash dump, and on some
    // platforms by other processes — and the secret has no business being
    // there once this process holds it.
#if __has_include(<unistd.h>)
    static_cast<void>(::unsetenv("KANBAN_TOKEN_SECRET"));
#endif

    const char* connectionString = std::getenv("KANBAN_DB");
    kanban::db::setup(connectionString != nullptr ? connectionString
                                                  : "DRIVER=SQLite3;Database=kanban.db;Timeout=5000");

    // `std::from_chars`, not `std::atoi`: `atoi` has no error channel at all,
    // so `KANBAN_PORT=abc` would silently bind port 0 (a kernel-assigned
    // ephemeral port — the server comes up on an address no client was told
    // about) and `KANBAN_PORT=99999` would silently wrap to a different port
    // on the cast to `quint16`. Both are worse than not starting: an
    // operator who mistyped the port gets a server that *looks* healthy.
    // Parsed before `App` is constructed so a bad value costs nothing.
    quint16 port = 8768;
    if (const char* portEnv = std::getenv("KANBAN_PORT"); portEnv != nullptr) {
        const std::string_view text{portEnv};
        std::uint16_t parsed = 0;
        const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), parsed);
        if (ec != std::errc{} || end != text.data() + text.size()) {
            std::cerr << "kanban-server: KANBAN_PORT='" << portEnv << "' is not a valid port number (0-65535)\n";
            return 2;
        }
        port = parsed;
    }

    int exitCode = 0;
    {
        // Create and install the TokenIssuer before App constructs RemoteServer,
        // which installs KanbanAuthorizer.
        auto issuer = std::make_shared<::morph::session::TokenIssuer>(
            tokenSecret, ::morph::session::hmacSha256);
        kanban::auth::setTokenIssuer(issuer);

        kanban::app::App app{std::filesystem::current_path() / "kanban_actions.jsonl"};

        ::morph::qt::QtWebSocketServer wsServer{*app.server(), port};
        if (!wsServer.listen()) {
            std::cerr << "kanban-server: failed to listen on port " << port << "\n";
            return 1;
        }
        std::cout << "kanban-server: listening on ws://127.0.0.1:" << wsServer.port() << std::endl;

        std::signal(SIGINT, onStopSignal);
        std::signal(SIGTERM, onStopSignal);
        QTimer stopPoll;
        QObject::connect(&stopPoll, &QTimer::timeout, &qtApp, [] {
            if (gStopRequested != 0) {
                QCoreApplication::quit();
            }
        });
        stopPoll.start(std::chrono::milliseconds{200});

        exitCode = QCoreApplication::exec();

        // Let connected clients' in-flight executes reply and close cleanly
        // before `app` leaves this scope. Unlike bookmarks' server, there is
        // no background worker to drain afterward: `kanban::app::App` is
        // plain C++ with no timer at all (see its own `@file` comment) —
        // every mutation this rung's `BoardModel` performs is synchronous,
        // inside the calling `execute()`, so there is nothing left in flight
        // once every client connection has closed.
        static_cast<void>(wsServer.closeGracefully(std::chrono::seconds{2}));
    }

    std::cout << "kanban-server: stopped\n";
    return exitCode;
}

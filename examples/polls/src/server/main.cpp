// SPDX-License-Identifier: Apache-2.0

/// @file
/// polls' standalone server process: `polls::db::setup()` once, one
/// `polls::app::App` (worker pool + `RemoteServer` with a real
/// `auth::PollsAuthorizer` + durable action log), and one
/// `morph::qt::QtWebSocketServer` in front of it. Mirrors
/// `bookmarks::src::server::main.cpp` closely, minus everything that server
/// owns and this rung has no equivalent for: there is no
/// `POLLS_TOKEN_SECRET` (this rung mints no process-wide signed tokens at
/// all -- `CreatePoll` generates bare admin/participant tokens per poll,
/// directly inside `PollModel::execute()`, see
/// `polls/auth/polls_authorizer.hpp`'s own `@file` comment), and there is no
/// background worker to drain on shutdown (`polls::app::App` is plain C++
/// with no timer at all -- see that header's own `@file` comment).
///
/// Usage:
/// @code
/// POLLS_DB=... POLLS_PORT=8767 ladder_polls_server
/// @endcode

#include <QCoreApplication>
#include <QTimer>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <morph/qt/qt_websocket_server.hpp>
#include <string_view>
#include <system_error>

#include "polls/app/app.hpp"
#include "polls/db/database.hpp"

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
        std::cerr << "polls-server: unknown argument '" << argv[i] << "' (usage: ladder_polls_server)\n";
        return 2;
    }

    const char* connectionString = std::getenv("POLLS_DB");
    polls::db::setup(connectionString != nullptr ? connectionString : "DRIVER=SQLite3;Database=polls.db;Timeout=5000");

    // `std::from_chars`, not `std::atoi`: `atoi` has no error channel at all,
    // so `POLLS_PORT=abc` would silently bind port 0 (a kernel-assigned
    // ephemeral port — the server comes up on an address no client was told
    // about) and `POLLS_PORT=99999` would silently wrap to a different port
    // on the cast to `quint16`. Both are worse than not starting: an
    // operator who mistyped the port gets a server that *looks* healthy.
    // Parsed before `App` is constructed so a bad value costs nothing.
    quint16 port = 8767;
    if (const char* portEnv = std::getenv("POLLS_PORT"); portEnv != nullptr) {
        const std::string_view text{portEnv};
        std::uint16_t parsed = 0;
        const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), parsed);
        if (ec != std::errc{} || end != text.data() + text.size()) {
            std::cerr << "polls-server: POLLS_PORT='" << portEnv << "' is not a valid port number (0-65535)\n";
            return 2;
        }
        port = parsed;
    }

    int exitCode = 0;
    {
        polls::app::App app{std::filesystem::current_path() / "polls_actions.jsonl"};

        ::morph::qt::QtWebSocketServer wsServer{*app.server(), port};
        if (!wsServer.listen()) {
            std::cerr << "polls-server: failed to listen on port " << port << "\n";
            return 1;
        }
        std::cout << "polls-server: listening on ws://127.0.0.1:" << wsServer.port() << std::endl;

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
        // no background worker to drain afterward: `polls::app::App` is
        // plain C++ with no timer at all (see its own `@file` comment) —
        // every mutation this rung's `PollModel` performs is synchronous,
        // inside the calling `execute()`, so there is nothing left in flight
        // once every client connection has closed.
        static_cast<void>(wsServer.closeGracefully(std::chrono::seconds{2}));
    }

    std::cout << "polls-server: stopped\n";
    return exitCode;
}

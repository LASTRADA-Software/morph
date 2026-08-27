// SPDX-License-Identifier: Apache-2.0

/// @file
/// ledger's standalone server process: `ledger::db::setup()` once, one
/// `ledger::app::App` (worker pool + `RemoteServer` with a real
/// `LedgerAuthorizer` + the process-global `TokenIssuer` + the periodic
/// report runner), and one `morph::qt::QtWebSocketServer` in front of it. The
/// desktop client (`examples/ledger/gui/`) talks to this over
/// `ws://127.0.0.1:<port>`; nothing here knows anything about ledger at all
/// -- `app.cpp` includes every model header deliberately so a `main()` that
/// names only `App` still links and serves all four models plus `AuthModel`.
/// Mirrors `bookmarks::app`'s server `main.cpp` closely (same shutdown
/// signal handling, same required-secret-with-no-default posture); this rung
/// has no metadata-fetch worker or outbox relay to drain on shutdown, so
/// there is no equivalent of `drainMetadataFetches` here -- only the report
/// runner's own `stopBackgroundJobs()`/`reportsInFlight()` pair.
///
/// Usage:
/// @code
/// LEDGER_TOKEN_SECRET=... LEDGER_DB=... LEDGER_PORT=8770 \
///     ladder_ledger_server
/// @endcode

#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <morph/qt/qt_websocket_server.hpp>
#include <string>
#include <string_view>
#include <system_error>

#include "ledger/app/app.hpp"
#include "ledger/db/database.hpp"

// `unsetenv` is POSIX, not <cstdlib>. This server target is only built for
// desktop platforms (morph_add_rung() does not emit it for WASM), all of
// which provide it.
#if __has_include(<unistd.h>)
#include <unistd.h>
#endif

namespace {

/// @brief Set from the `SIGINT`/`SIGTERM` handler, polled by a `QTimer`.
///
/// A signal handler may not call into Qt (nothing in `QCoreApplication` is
/// async-signal-safe), so it does the one thing it is allowed to do -- assign
/// to a `volatile std::sig_atomic_t` -- and a timer on the Qt thread turns
/// that into a real `quit()`. Identical in shape to bookmarks'/pastebin's own
/// server `main.cpp`.
volatile std::sig_atomic_t gStopRequested = 0;

extern "C" void onStopSignal(int /*signum*/) { gStopRequested = 1; }

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication qtApp{argc, argv};

    for (int i = 1; i < argc; ++i) {
        std::cerr << "ledger-server: unknown argument '" << argv[i]
                  << "' (usage: LEDGER_TOKEN_SECRET=... ladder_ledger_server)\n";
        return 2;
    }

    // Required, with no default: the secret signs every token this server
    // mints and verifies every token it is shown, so a built-in fallback
    // would be a published signing key. Refusing to start is the only honest
    // behavior (`docs/spec/security.md`).
    const char* tokenSecretEnv = std::getenv("LEDGER_TOKEN_SECRET");
    if (tokenSecretEnv == nullptr || *tokenSecretEnv == '\0') {
        std::cerr << "ledger-server: LEDGER_TOKEN_SECRET must be set to a non-empty value\n";
        return 2;
    }
    const std::string tokenSecret{tokenSecretEnv};
    // Cleared from the environment the moment it has been copied -- see
    // bookmarks' identical server main.cpp comment for the full rationale
    // (a crash dump or another `getenv` call has no business seeing this
    // once the process holds it).
#if __has_include(<unistd.h>)
    static_cast<void>(::unsetenv("LEDGER_TOKEN_SECRET"));
#endif

    const char* connectionString = std::getenv("LEDGER_DB");
    ledger::db::setup(connectionString != nullptr ? connectionString
                                                  : "DRIVER=SQLite3;Database=ledger.db;Timeout=5000");

    // `std::from_chars`, not `std::atoi` -- see bookmarks' identical server
    // main.cpp comment: atoi has no error channel, so a mistyped port would
    // silently bind somewhere the operator was never told about instead of
    // failing loudly.
    quint16 port = 8770;
    if (const char* portEnv = std::getenv("LEDGER_PORT"); portEnv != nullptr) {
        const std::string_view text{portEnv};
        std::uint16_t parsed = 0;
        const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), parsed);
        if (ec != std::errc{} || end != text.data() + text.size()) {
            std::cerr << "ledger-server: LEDGER_PORT='" << portEnv << "' is not a valid port number (0-65535)\n";
            return 2;
        }
        port = parsed;
    }

    int exitCode = 0;
    {
        ledger::app::App app{tokenSecret};

        ::morph::qt::QtWebSocketServer wsServer{*app.server(), port};
        if (!wsServer.listen()) {
            std::cerr << "ledger-server: failed to listen on port " << port << "\n";
            return 1;
        }
        std::cout << "ledger-server: listening on ws://127.0.0.1:" << wsServer.port() << std::endl;

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

        // Disarm the report timer first, before anything below spins the
        // event loop again -- the identical ordering rationale bookmarks'
        // server main.cpp documents for its own fetch/relay timers: a tick
        // delivered by the drain's own `processEvents()` would dispatch a
        // brand-new pass and re-raise `reportsInFlight()` after it had
        // settled.
        app.stopBackgroundJobs();

        // Let connected clients' in-flight executes reply and close cleanly
        // first, *then* drain the report runner's own dispatches before
        // `app` leaves this scope.
        static_cast<void>(wsServer.closeGracefully(std::chrono::seconds{2}));
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (app.reportsInFlight() && std::chrono::steady_clock::now() < deadline) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        }
        if (app.reportsInFlight()) {
            std::cerr << "ledger-server: report-runner dispatches did not settle within 5s; shutting down anyway\n";
        }
    }

    std::cout << "ledger-server: stopped\n";
    return exitCode;
}

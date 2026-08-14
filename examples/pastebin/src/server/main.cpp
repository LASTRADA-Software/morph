// SPDX-License-Identifier: Apache-2.0

/// @file
/// pastebin's standalone server process: `pastebin::db::setup()` once, one
/// `pastebin::app::App` (worker pool + `RemoteServer` + durable action log +
/// expiry sweep), and one `morph::qt::QtWebSocketServer` in front of it. The
/// desktop client (`examples/pastebin/gui/`) talks to this over
/// `ws://127.0.0.1:<port>`; nothing here knows anything about pastes beyond
/// the `--seed` demo data below, which is deliberately a handful of literal
/// `CreatePaste` values (`LADDER.md`'s "every rung ships a `--seed` path").
/// The generator machinery in `action_driver.hpp` is rung 4's deliverable
/// (`TESTING.md`'s component table) and is not pulled forward for it.
///
/// Usage:
/// @code
/// PASTEBIN_DB=... PASTEBIN_PORT=8765 ladder_pastebin_server [--seed]
/// @endcode

// examples/common is on every ladder target's include path as a root, so the
// ladder clock is "clock.hpp" — the same spelling paste_model.cpp and app.cpp
// use. Seeding reads the *same* injectable clock the model does, so a seeded
// expiry and the model's own expiry check can never disagree.
#include "clock.hpp"
#include "pastebin/app/app.hpp"
#include "pastebin/db/database.hpp"
#include "pastebin/dto/paste_dto.hpp"
#include "pastebin/models/paste_model.hpp"

#include <morph/qt/qt_websocket_server.hpp>
#include <morph/util/datetime.hpp>

#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

/// @brief Set from the `SIGINT`/`SIGTERM` handler, polled by a `QTimer`.
///
/// A signal handler may not call into Qt (nothing in `QCoreApplication` is
/// async-signal-safe), so it does the one thing it is allowed to do — assign
/// to a `volatile std::sig_atomic_t` — and a timer on the Qt thread turns that
/// into a real `quit()`. This exists so the shutdown path below is actually
/// *reachable*: a demo server is stopped with Ctrl-C, and the default `SIGINT`
/// disposition would terminate the process outright, so `exec()` would never
/// return and `App`'s destructor would never run at all.
volatile std::sig_atomic_t gStopRequested = 0;

extern "C" void onStopSignal(int /*signum*/) { gStopRequested = 1; }

/// @brief Pumps the Qt event loop until no sweep dispatch is outstanding.
///
/// `pastebin::app::App::sweepInFlight()` is observe-only: `~App` does *not*
/// wait for the `ExpirePaste` calls a sweep dispatched to settle before
/// destroying the bridge they complete against, so a callback delivered after
/// `~App` is a use-after-free. The header states the contract — "pump on this
/// until it is `false`, then destroy" — and this is the production consumer
/// honouring it. Bounded by @p budget so a wedged dispatch cannot hang
/// shutdown forever; overrunning it is strictly better than the alternative of
/// not draining at all, and is reported.
///
/// @param app    The app whose sweep dispatches must settle.
/// @param budget Maximum time to wait.
/// @return `true` if everything settled within @p budget.
[[nodiscard]] bool drainSweeps(const pastebin::app::App& app, std::chrono::milliseconds budget) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (app.sweepInFlight()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return true;
}

/// @brief Creates the demo corpus, in-process and synchronously.
///
/// Calls `PasteModel::execute()` directly rather than going through a
/// `Bridge`/`BridgeHandler`: seeding happens before the listener starts, on
/// the Qt thread, with nothing to dispatch to and nobody to be concurrent
/// with. The model is the application (`IMPLEMENTATION.md` rule 1), so a
/// direct call runs exactly the same id allocation, clamping and persistence
/// a client-issued `CreatePaste` would — only the transport is skipped.
void seedDemoPastes() {
    using namespace std::chrono_literals;
    pastebin::PasteModel model;

    const auto create = [&model](pastebin::CreatePaste action, const char* what) {
        try {
            const auto result = model.execute(action);
            std::cout << "pastebin-server: seeded " << what << " as "
                      << (result.id.hasValue() ? *result.id : std::string{"<none>"}) << '\n';
        } catch (const std::exception& e) {
            std::cerr << "pastebin-server: failed to seed " << what << ": " << e.what() << '\n';
        }
    };

    create({.content = "Hello from the morph application ladder, rung 1.", .syntax = "plaintext"},
           "a plain public paste");
    create({.content = "int main() { return 0; }", .syntax = "cpp", .editability = pastebin::Editability::Editable},
           "an editable C++ snippet");
    create({.content = "SELECT id, syntax FROM pastes ORDER BY created_at_ms DESC;", .syntax = "sql"},
           "a SQL snippet");
    create({.content = "This paste is private; it never shows up in ListPastes.",
            .syntax = "plaintext",
            .visibility = pastebin::Visibility::Private},
           "a private paste");
    create({.content = "One read and this is gone. Open it twice to see the burn.",
            .syntax = "plaintext",
            .burnAfterReads = pastebin::Reads::fromDouble(1.0)},
           "a burn-after-1 paste");
    create({.content = "This one expires two minutes after the server started.",
            .syntax = "plaintext",
            .expiresAt = ::morph::time::Timestamp{*::morph::ladder::now() + 2min}},
           "a paste expiring in two minutes");
}

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication qtApp{argc, argv};

    bool seed = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg{argv[i]};
        if (arg == "--seed") {
            seed = true;
        } else {
            std::cerr << "pastebin-server: unknown argument '" << arg
                      << "' (usage: ladder_pastebin_server [--seed])\n";
            return 2;
        }
    }

    const char* connectionString = std::getenv("PASTEBIN_DB");
    pastebin::db::setup(connectionString != nullptr ? connectionString
                                                    : "DRIVER=SQLite3;Database=pastebin.db;Timeout=5000");

    if (seed) {
        seedDemoPastes();
    }

    int exitCode = 0;
    {
        pastebin::app::App app{std::filesystem::current_path() / "pastebin_actions.jsonl"};

        const char* portEnv = std::getenv("PASTEBIN_PORT");
        const int port = portEnv != nullptr ? std::atoi(portEnv) : 0;
        ::morph::qt::QtWebSocketServer wsServer{*app.server(), static_cast<quint16>(port)};
        if (!wsServer.listen()) {
            std::cerr << "pastebin-server: failed to listen\n";
            return 1;
        }
        std::cout << "pastebin-server: listening on port " << wsServer.port() << std::endl;

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

        // Order matters: let connected clients' in-flight executes reply and
        // close cleanly first, *then* drain the expiry sweep's own dispatches
        // (see drainSweeps) before `app` leaves this scope.
        static_cast<void>(wsServer.closeGracefully(std::chrono::seconds{2}));
        if (!drainSweeps(app, std::chrono::seconds{5})) {
            std::cerr << "pastebin-server: expiry-sweep dispatches did not settle within 5s; "
                         "shutting down anyway\n";
        }
    }

    std::cout << "pastebin-server: stopped\n";
    return exitCode;
}

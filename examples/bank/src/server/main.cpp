// SPDX-License-Identifier: Apache-2.0

/// @file
/// bank's standalone server process: `bank::db::setup()` once, one process-wide
/// `morph::journal::FileActionLog`, one `morph::backend::RemoteServer` over a
/// worker pool, and one `morph::qt::QtWebSocketServer` in front of it. Speaks
/// the same `morph::wire` envelope protocol every `ladder_<rung>_server` does,
/// so `scripts/scenario/run_scenarios.py --rung bank` can drive bank's models
/// as a real out-of-process client.
///
/// Usage:
/// @code
/// BANK_DB="DRIVER=SQLite3;Database=bank.db;Timeout=5000" BANK_PORT=0 \
///     ladder_bank_server [--mcp-stdio]
/// @endcode
///
/// The server also serves bank's screens through a `morph::ui::UiService`, as the
/// other rung servers do, so the generic client can open it with `--server`.
///
/// @par `--mcp-stdio`
/// Serves an agent over MCP's stdio transport beside the WebSocket clients: a
/// `morph::ui::McpAdapter` on a connection of its own, its tool calls running as
/// ordinary executes, so every GUI watching what they change is told. Stdout then
/// carries only JSON-RPC, so the status lines go to stderr. The agent's identity
/// is `MORPH_MCP_PRINCIPAL`, which this demo authorizer vouches for, and
/// `MORPH_MCP_TOKEN`, carried as its token for an authorizer that verifies one.
/// When stdin ends the server stops.
///
/// @par Why this `main()` owns the server rather than `bank::app::App`
/// Every ladder rung puts its `RemoteServer` in an `App` and lets `main()` name
/// only that class. Bank cannot follow that shape without changing what
/// `bank::app::App` *is*: that class is the **client**-side context the GUI and
/// the CLI build on — a `ThreadPoolExecutor`, a `MainThreadExecutor` for
/// callbacks, and one `Bridge` over a `LocalBackend`, plus the
/// `login()`/`logout()` pair that sets that bridge's default session
/// (`examples/bank/src/app/app.cpp`). It hosts no `RemoteServer` and needs
/// none; adding one would put a listener's worth of machinery into every
/// desktop and CLI process that constructs an `App` purely so a fourth
/// consumer could reach it. The server side bank actually needs is small
/// enough to live here in full, so it does.
///
/// @par Why every model header is included
/// `BRIDGE_REGISTER_MODEL`/`BRIDGE_REGISTER_ACTION` place their registrars in
/// the *header*, so a translation unit that includes one both registers the
/// type with the process-wide registry and emits a reference to that model's
/// `execute` bodies — which is what pulls the model's object file out of
/// `bank_lib` for a binary whose own code names none of them. Without these
/// includes this server would link and then come up serving nothing. The rung
/// servers get the same effect from their `App` translation unit; bank has no
/// such file on the server side, so the includes are here. Any model added to
/// `bank_lib` must be added to this list too, or it is silently unreachable
/// over the wire.
///
/// @par No `--seed`
/// Every bank action is scoped to `session::current()->principal`, and the
/// principal is only meaningful once a `users` row exists for it — which is
/// `RegisterUser`'s job. Seeding by calling models directly would therefore
/// have to install a thread-local session itself, reaching into
/// `morph::session::detail::ScopedContext`; `bookmarks`' server declines to do
/// that for the same reason and so does this one. Demo data is created through
/// the client, which is also the path a real user takes.

#include <QCoreApplication>
#include <QMetaObject>
#include <QTimer>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <morph/core/executor.hpp>
#include <morph/core/remote.hpp>
#include <morph/journal/file_action_log.hpp>
#include <morph/journal/journal.hpp>
#include <morph/qt/qt_executor.hpp>
#include <morph/qt/qt_websocket_server.hpp>
#include <morph/session/session.hpp>
#include <morph/ui/agent.hpp>
#include <morph/ui/service.hpp>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <thread>

#include "bank/db/database.hpp"
#include "bank_screens.hpp"

// Every model bank_lib hosts — see this file's "Why every model header is
// included" note. Sorted, so a missing one is visible by reading.
#include "bank/models/account_model.hpp"
#include "bank/models/auth_model.hpp"
#include "bank/models/budget_model.hpp"
#include "bank/models/card_model.hpp"
#include "bank/models/customer_model.hpp"
#include "bank/models/loan_model.hpp"
#include "bank/models/notification_model.hpp"
#include "bank/models/payee_model.hpp"
#include "bank/models/payment_model.hpp"
#include "bank/models/statement_model.hpp"
#include "bank/models/transaction_model.hpp"

namespace {

/// @brief Set from the `SIGINT`/`SIGTERM` handler, polled by a `QTimer`.
///
/// A signal handler may not call into Qt (nothing in `QCoreApplication` is
/// async-signal-safe), so it does the one thing it is allowed to do — assign to
/// a `volatile std::sig_atomic_t` — and a timer on the Qt thread turns that
/// into a real `quit()`. Same shape, and the same reason, as
/// `examples/bookmarks/src/server/main.cpp`: without it the default `SIGINT`
/// disposition would terminate the process outright and the shutdown path below
/// would never run.
volatile std::sig_atomic_t gStopRequested = 0;

extern "C" void onStopSignal(int /*signum*/) { gStopRequested = 1; }

/// @brief Vouches for a client's asserted principal, and refuses an empty one.
///
/// **This is a demo authorizer and is not authentication.** It exists because
/// of a real property of bank's domain, not as a shortcut: bank's `AuthModel`
/// mints no bearer token. `LoginRequest` verifies a password and returns the
/// principal to install (`dto::AuthResult::principal`), and the *client* is
/// what installs it — `bank::app::App::login()` calls
/// `Bridge::setDefaultSession` with it. There is no signed artefact for a
/// server to verify, so a `SigningAuthorizer` (what `bookmarks` and `lims`
/// install) would refuse every client bank ships, and inventing a
/// token for the server alone would mean changing `AuthModel`'s wire results —
/// a change to bank's GUI and CLI, made to suit a test transport.
///
/// The default `allowAllAuthorizer()` is not an option either, and this is the
/// subtle part: `RemoteServer::stampVerifiedPrincipal` *clears*
/// `session.principal` whenever `authenticate()` returns `nullopt`
/// (`include/morph/core/remote.hpp`), so under the default authorizer every
/// bank action would run with an empty principal and fail with
/// `"no session principal"`. Bank would be registered, reachable, and unable
/// to do anything. Vouching for the claim is what makes the wire behave the
/// way bank's own in-process client already does.
///
/// What this deliberately keeps: an *empty* principal is not vouched for, so
/// `RemoteServer` clears it and unauthenticated calls are refused by the models
/// exactly as they are in-process. And it grants no ownership: every bank model
/// still resolves ownership per row (`db::loadOwned`, `db::loadOwnedOpenAccount`
/// in `examples/bank/include/bank/db/ledger_ops.hpp`), so naming another user's
/// principal is refused by the model even though the authorizer accepted the
/// name. Cross-principal isolation is therefore genuinely testable over this
/// transport; credential *proof* is not, because bank has none to prove.
///
/// A production deployment replaces this with `morph::session::SigningAuthorizer`
/// over a token `AuthModel` issues. See `docs/spec/security.md`.
struct BankDemoAuthorizer : ::morph::session::IAuthorizer {
    /// @brief Accepts any action; ownership is enforced per row by the models.
    /// @return Always `true`.
    [[nodiscard]] bool authorize(const ::morph::session::Context& /*ctx*/, std::string_view /*model*/,
                                 std::string_view /*actionType*/) const override {
        return true;
    }

    /// @brief Vouches for a non-empty asserted principal; refuses an empty one.
    /// @param ctx The client's claimed session.
    /// @return The claimed principal, or `std::nullopt` when it is empty.
    [[nodiscard]] std::optional<std::string> authenticate(const ::morph::session::Context& ctx) const override {
        if (ctx.principal.empty()) {
            return std::nullopt;
        }
        return ctx.principal;
    }
};

/// @brief Live-instance cap this server installs.
///
/// Bank hosts eleven models and this authorizer installs no `authorizeRegister`
/// override, so an unauthenticated client can still make the server create
/// instances even though it can execute nothing on them. Generous on purpose:
/// a client registering all eleven models still fits many times over, so this
/// is a bound on abuse rather than a limit a real session meets. Mirrors
/// `bookmarks::app::App`'s identical constant and rationale.
constexpr std::size_t kMaxLiveModels = 256;

/// @brief Reads a port from the environment, defaulting to `0` (OS picks).
/// @param name Environment variable to read.
/// @return The parsed port, or `0` when unset or unparseable.
[[nodiscard]] quint16 portFromEnvironment(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr) {
        return 0;
    }
    return static_cast<quint16>(std::atoi(value));
}

}  // namespace

// NOLINTNEXTLINE(bugprone-exception-escape): an example server; an uncaught failure at startup ends the process, which is the right outcome.
int main(int argc, char** argv) {
    QCoreApplication qtApp{argc, argv};

    bool mcpStdio = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string_view{argv[i]} == "--mcp-stdio") {
            mcpStdio = true;
            continue;
        }
        std::cerr << "bank-server: unknown argument '" << argv[i] << "' (usage: BANK_DB=... BANK_PORT=... "
                  << "ladder_bank_server [--mcp-stdio])\n";
        return 2;
    }
    // With --mcp-stdio, stdout is the agent's JSON-RPC channel and nothing else may be written to it.
    std::ostream& status = mcpStdio ? std::cerr : std::cout;

    const char* connectionString = std::getenv("BANK_DB");
    try {
        bank::db::setup(connectionString != nullptr ? connectionString
                                                    : "DRIVER=SQLite3;Database=bank.db;Timeout=5000");
    } catch (const std::exception& e) {
        std::cerr << "bank-server: could not open the database: " << e.what() << '\n';
        return 1;
    }

    // Bank's CLI driver installs exactly this (examples/bank/src/cli/main.cpp),
    // and the models are already annotated for it: the read-only actions carry
    // `Loggable::No`, so what lands in the journal is the mutating half of the
    // surface and not every list call. Installed process-wide, because a
    // registry-constructed model is always default-constructed and so has no
    // DI seam to receive it through.
    // The log belongs to this thread's event loop: models append from their
    // strands on the pool below, and every append runs here, one at a time.
    ::morph::qt::QtExecutor journalOwner;
    auto actionLog = std::make_shared<::morph::journal::FileActionLog>(
        journalOwner, std::filesystem::current_path() / "bank_actions.jsonl");
    ::morph::journal::setActionLog(actionLog);

    int exitCode = 0;
    {
        auto bundle = bank::screens::bundle();
        if (!bundle) {
            std::cerr << "bank-server: the screens do not build: " << bundle.error().message() << '\n';
            ::morph::journal::setActionLog(nullptr);
            return 1;
        }
        ::morph::exec::ThreadPoolExecutor pool{4};
        ::morph::backend::ServerConfig serverConfig;
        serverConfig.limits.maxLiveModels = kMaxLiveModels;
        serverConfig.services.push_back(std::make_shared<::morph::ui::UiService>(*bundle));
        auto server = std::make_shared<::morph::backend::RemoteServer>(pool, std::make_shared<BankDemoAuthorizer>(),
                                                                       std::move(serverConfig));

        ::morph::qt::QtWebSocketServer wsServer{*server, portFromEnvironment("BANK_PORT")};
        if (!wsServer.listen()) {
            std::cerr << "bank-server: failed to listen\n";
            ::morph::journal::setActionLog(nullptr);
            return 1;
        }
        // The one line scripts/scenario/run_scenarios.py parses to learn the
        // port (its `PORT_LINE`); `std::endl` because that tool reads this
        // pipe line by line and a buffered announcement would hang it.
        status << "bank-server: listening on ws://127.0.0.1:" << wsServer.port() << std::endl;

        // The agent's own thread: it owns the adapter and reads stdin, and stops the server once stdin ends.
        std::thread mcp;
        if (mcpStdio) {
            mcp = std::thread{[server] {
                ::morph::session::Context session;
                if (char const* const principal = std::getenv("MORPH_MCP_PRINCIPAL")) {
                    session.principal = principal;
                }
                if (char const* const token = std::getenv("MORPH_MCP_TOKEN")) {
                    session.token = token;
                }
                {
                    ::morph::exec::MainThreadExecutor owner;
                    ::morph::ui::McpAdapter adapter{
                        server, ::morph::ui::McpOptions{.session = session, .serverName = "bank", .callbacks = &owner}};
                    ::morph::ui::serveStdio(adapter, owner, std::cin, std::cout);
                }
                QMetaObject::invokeMethod(QCoreApplication::instance(), [] { QCoreApplication::quit(); },
                                          Qt::QueuedConnection);
            }};
        }

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
        // before `server` and `pool` leave this scope. Bank runs no background
        // job, so there is no sweep to drain afterwards — a rung server that
        // does (bookmarks' metadata-fetch worker) needs a second step here and
        // bank does not.
        static_cast<void>(wsServer.closeGracefully(std::chrono::seconds{2}));
        if (mcp.joinable()) {
            if (gStopRequested != 0) {
                // Stopped by a signal while the agent's stdin is still open: its read cannot be interrupted, so the
                // process ends here rather than wait for an input that may never come.
                ::morph::journal::setActionLog(nullptr);
                std::_Exit(exitCode);
            }
            mcp.join();
        }
    }

    // Matches the clear-on-shutdown discipline every rung App follows: nothing
    // should observe a live log belonging to a process that has stopped.
    ::morph::journal::setActionLog(nullptr);

    status << "bank-server: stopped\n";
    return exitCode;
}

// SPDX-License-Identifier: Apache-2.0

/// @file
/// bookmarks' standalone server process: `bookmarks::db::setup()` once, one
/// `bookmarks::app::App` (worker pool + `RemoteServer` with a real
/// `BookmarksAuthorizer` + durable action log + the process-global
/// `TokenIssuer` + the metadata-fetch worker + the outbox relay), and one
/// `morph::qt::QtWebSocketServer` in front of it. The desktop client
/// (`examples/bookmarks/gui/`) talks to this over `ws://127.0.0.1:<port>`;
/// nothing here knows anything about bookmarks at all — `app.cpp` includes
/// every model header deliberately so a `main()` that names only `App` still
/// links and serves all four models.
///
/// Usage:
/// @code
/// BOOKMARKS_TOKEN_SECRET=... BOOKMARKS_DB=... BOOKMARKS_PORT=8766 \
///     ladder_bookmarks_server
/// @endcode
///
/// `BOOKMARKS_TRANSPORT` picks what serves clients: `qt` (default,
/// `QtWebSocketServer`) or `net` (`morph::net::SocketServer`, where built).
/// `BOOKMARKS_STORE` picks where bookmarks live: `sql` (default, the
/// database) or `memory` (each model instance's own memory, gone with it).
/// `BOOKMARKS_WORKERS` sizes the model worker pool (default 4) and
/// `BOOKMARKS_MAX_LIVE_MODELS` caps live model instances (default
/// `bookmarks::app::kDefaultMaxLiveModels`; `0` is unbounded).
///
/// @par No `--seed`, and why
/// This server ships none, deliberately. Every action in this rung is
/// scoped to `session::current()->principal`, so seeding by calling a model
/// directly would have
/// to install a thread-local session itself, i.e. reach into
/// `morph::session::detail::ScopedContext`, a `detail::` namespace with no
/// public seam for this — exactly the class of reach-in
/// `examples/common/testkit` migrated away from onto public seams
/// (`Completion<T>::makeSettleable()`, the
/// `QtWebSocketBackend(url, tls, cfg)` overload) as soon as public seams
/// existed for them; adding a new reach-in here from an *example* would be a
/// step backward, not forward. The alternative — an internal client with a
/// minted service token, the shape `App`'s own metadata worker uses — is
/// real infrastructure that belongs with the testkit's `action_driver`
/// generators, not in a server main. Demo data is therefore created through the
/// client, which also exercises the path a user actually takes.

#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>
#include <bookmarks_screens.hpp>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <expected>
#include <filesystem>
#include <iostream>
#include <memory>
#include <morph/qt/qt_websocket_server.hpp>
#include <optional>
#if defined(BOOKMARKS_HAVE_NET)
#include <morph/net/socket_server.hpp>
#endif
#include <morph/ui/service.hpp>
#include <string>
#include <string_view>
#include <system_error>

#include "bookmarks/app/app.hpp"
#include "bookmarks/db/database.hpp"
#include "bookmarks/models/bookmark_model.hpp"

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
/// async-signal-safe), so it does the one thing it is allowed to do — assign
/// to a `volatile std::sig_atomic_t` — and a timer on the Qt thread turns that
/// into a real `quit()`. This exists so the shutdown path below is actually
/// *reachable*: a demo server is stopped with Ctrl-C, and the default `SIGINT`
/// disposition would terminate the process outright, so `exec()` would never
/// return and `App`'s destructor would never run at all.
volatile std::sig_atomic_t gStopRequested = 0;

extern "C" void onStopSignal(int /*signum*/) { gStopRequested = 1; }

/// @brief Pumps the Qt event loop until no metadata-fetch dispatch is
///        outstanding.
///
/// `bookmarks::app::App::fetchInFlight()` is observe-only and its header
/// states the contract explicitly — "pump on this until it is `false`, then
/// destroy" — because `~App` does *not* wait for the `RecordMetadata` calls a
/// pass dispatched to settle before destroying the bridge they complete
/// against. This task's brief said no drain step was needed here, on the
/// grounds that `fetchInFlight()` is a test-only concern; that is not what the
/// header says, and it is not true of a *server*: the fetch timer fires every
/// five seconds by default, so a `SIGTERM` landing mid-pass is an ordinary
/// event, not an exotic one. The drain is therefore kept. Bounded by
/// @p budget so a wedged dispatch cannot hang shutdown forever; overrunning it
/// is strictly better than not draining at all, and is reported.
///
/// The outbox relay needs no equivalent: `relayOutboxOnce()` is synchronous —
/// it touches the database and the log directly rather than dispatching
/// through the server — so there is never anything of its own in flight.
///
/// @pre `app.stopBackgroundJobs()` has already been called. This loop's own
/// `processEvents()` is what delivers @p app's fetch-timer ticks, so with the
/// timer still armed the drain would race the very thing it is draining — see
/// `App::stopBackgroundJobs()`'s doc comment for the full sequence.
///
/// @param app    The app whose metadata dispatches must settle.
/// @param budget Maximum time to wait.
/// @return `true` if everything settled within @p budget.
[[nodiscard]] bool drainMetadataFetches(const bookmarks::app::App& app, std::chrono::milliseconds budget) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (app.fetchInFlight()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return true;
}

}  // namespace

namespace {

/// @brief The UI service over bookmarks's screens, served beside its models so a generic client opens them (spec 4
/// §1).
/// @return The service, or why the screens do not build.
std::expected<std::shared_ptr<::morph::backend::IEnvelopeService>, std::string> screensService() {
    auto bundle = bookmarks::screens::bundle();
    if (!bundle) {
        return std::unexpected(bundle.error().message());
    }
    return std::make_shared<::morph::ui::UiService>(std::move(*bundle));
}

}  // namespace

namespace {

/// @brief Reads a non-negative integer from environment variable @p name.
/// @param name     The variable.
/// @param fallback The value when the variable is unset.
/// @return The value, or `std::nullopt` (after saying why) when it is set but is not a whole number.
[[nodiscard]] std::optional<std::size_t> sizeFromEnv(const char* name, std::size_t fallback) {
    const char* raw = std::getenv(name);
    if (raw == nullptr) {
        return fallback;
    }
    const std::string_view text{raw};
    std::size_t parsed = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (text.empty() || ec != std::errc{} || end != text.data() + text.size()) {
        std::cerr << "bookmarks-server: " << name << "='" << raw << "' is not a non-negative whole number\n";
        return std::nullopt;
    }
    return parsed;
}

/// @brief The server: everything `main` does, with what it throws left to `main`.
/// @param argc The argument count.
/// @param argv The arguments.
/// @return The exit code.
int run(int argc, char** argv) {
    QCoreApplication qtApp{argc, argv};

    for (int i = 1; i < argc; ++i) {
        std::cerr << "bookmarks-server: unknown argument '" << argv[i]
                  << "' (usage: BOOKMARKS_TOKEN_SECRET=... ladder_bookmarks_server)\n";
        return 2;
    }

    // Required, with no default: the secret signs every token this server
    // mints and verifies every token it is shown, so a built-in fallback
    // would be a published signing key. Refusing to start is the only honest
    // behavior (`docs/spec/security.md`).
    const char* tokenSecretEnv = std::getenv("BOOKMARKS_TOKEN_SECRET");
    if (tokenSecretEnv == nullptr || *tokenSecretEnv == '\0') {
        std::cerr << "bookmarks-server: BOOKMARKS_TOKEN_SECRET must be set to a non-empty value\n";
        return 2;
    }
    const std::string tokenSecret{tokenSecretEnv};
    // Cleared from the environment the moment it has been copied. The
    // environment block is readable for the process's whole lifetime — by
    // anything that later calls `getenv`, by a crash dump, and on some
    // platforms by other processes — and the secret has no business being
    // there once this process holds it. `App` receives it by value, so
    // nothing below reads the variable again. Guarded by the same
    // `__has_include` check as the `<unistd.h>` include above: on a
    // hypothetical desktop platform without it, this degrades to leaving
    // the variable set rather than failing to compile.
#if __has_include(<unistd.h>)
    static_cast<void>(::unsetenv("BOOKMARKS_TOKEN_SECRET"));
#endif

    const char* connectionString = std::getenv("BOOKMARKS_DB");
    bookmarks::db::setup(connectionString != nullptr ? connectionString
                                                     : "DRIVER=SQLite3;Database=bookmarks.db;Timeout=5000");

    // `std::from_chars`, not `std::atoi`: `atoi` has no error channel at all,
    // so `BOOKMARKS_PORT=abc` would silently bind port 0 (a kernel-assigned
    // ephemeral port — the server comes up on an address no client was told
    // about) and `BOOKMARKS_PORT=99999` would silently wrap to 34463 on the
    // cast to `quint16`. Both are worse than not starting: an operator who
    // mistyped the port gets a server that *looks* healthy. Failing loudly
    // matches how BOOKMARKS_TOKEN_SECRET above already treats a bad value.
    // Parsed before `App` is constructed so a bad value costs nothing.
    quint16 port = 8766;
    if (const char* portEnv = std::getenv("BOOKMARKS_PORT"); portEnv != nullptr) {
        const std::string_view text{portEnv};
        std::uint16_t parsed = 0;
        const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), parsed);
        if (ec != std::errc{} || end != text.data() + text.size()) {
            std::cerr << "bookmarks-server: BOOKMARKS_PORT='" << portEnv << "' is not a valid port number (0-65535)\n";
            return 2;
        }
        port = parsed;
    }
    const auto workers = sizeFromEnv("BOOKMARKS_WORKERS", 4);
    const auto maxLiveModels = sizeFromEnv("BOOKMARKS_MAX_LIVE_MODELS", bookmarks::app::kDefaultMaxLiveModels);
    if (!workers || !maxLiveModels) {
        return 2;
    }
    const std::string transport =
        std::getenv("BOOKMARKS_TRANSPORT") != nullptr ? std::getenv("BOOKMARKS_TRANSPORT") : "qt";
#if defined(BOOKMARKS_HAVE_NET)
    if (transport != "qt" && transport != "net") {
        std::cerr << "bookmarks-server: BOOKMARKS_TRANSPORT='" << transport << "' is neither 'qt' nor 'net'\n";
        return 2;
    }
#else
    if (transport != "qt") {
        std::cerr
            << "bookmarks-server: BOOKMARKS_TRANSPORT='" << transport
            << "' is not available: this build has only the 'qt' transport (configure with MORPH_BUILD_NET=ON)\n";
        return 2;
    }
#endif
    const std::string store = std::getenv("BOOKMARKS_STORE") != nullptr ? std::getenv("BOOKMARKS_STORE") : "sql";
    if (store != "sql" && store != "memory") {
        std::cerr << "bookmarks-server: BOOKMARKS_STORE='" << store << "' is neither 'sql' nor 'memory'\n";
        return 2;
    }
    bookmarks::setStorage(store == "memory" ? bookmarks::Storage::Memory : bookmarks::Storage::Sql);
    if (*workers == 0) {
        std::cerr << "bookmarks-server: BOOKMARKS_WORKERS must be at least 1\n";
        return 2;
    }

    int exitCode = 0;
    {
        auto screens = screensService();
        if (!screens) {
            std::cerr << "bookmarks-server: the screens do not build: " << screens.error() << '\n';
            return 1;
        }
        bookmarks::app::App app{std::filesystem::current_path() / "bookmarks_actions.jsonl",
                                tokenSecret,
                                std::make_shared<bookmarks::app::NullMetadataFetcher>(),
                                std::chrono::seconds{5},
                                std::chrono::seconds{2},
                                *workers,
                                nullptr,
                                {std::move(*screens)},
                                *maxLiveModels};

        // One transport serves the clients: Qt WebSockets on this thread's event
        // loop, or, where morph::net is built, morph's own socket server on an
        // I/O loop of its own. The models, the journal and the timers are the
        // same either way.
        std::optional<::morph::qt::QtWebSocketServer> qtServer;
#if defined(BOOKMARKS_HAVE_NET)
        std::optional<::morph::net::SocketServer> netServer;
        if (transport == "net") {
            netServer.emplace(*app.server(), port);
        } else
#endif
        {
            qtServer.emplace(*app.server(), port);
        }
        bool listening = false;
        std::uint16_t boundPort = 0;
        if (qtServer) {
            listening = qtServer->listen();
            boundPort = qtServer->port();
        }
#if defined(BOOKMARKS_HAVE_NET)
        if (netServer) {
            listening = netServer->listen();
            boundPort = netServer->port();
        }
#endif
        if (!listening) {
            std::cerr << "bookmarks-server: failed to listen on port " << port << "\n";
            return 1;
        }
        std::cout << "bookmarks-server: listening on ws://127.0.0.1:" << boundPort << std::endl;

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

        // First, before anything below spins the event loop again: disarm the
        // periodic timers. Both `closeGracefully` and `drainMetadataFetches`
        // pump events, and a fetch tick delivered by one of *their*
        // `processEvents()` calls would start a whole new `RecordMetadata`
        // pass — re-raising `fetchInFlight()` after the drain had watched it
        // settle, and potentially leaving a dispatch outstanding when the
        // drain's budget expires and `app` is destroyed anyway. With the timer
        // stopped the drain is monotonic: the outstanding set only shrinks.
        app.stopBackgroundJobs();

        // Order matters: let connected clients' in-flight executes reply and
        // close cleanly first, *then* drain the metadata worker's own
        // dispatches (see drainMetadataFetches) before `app` leaves this
        // scope.
        if (qtServer) {
            static_cast<void>(qtServer->closeGracefully(std::chrono::seconds{2}));
        }
#if defined(BOOKMARKS_HAVE_NET)
        if (netServer) {
            netServer->close();
        }
#endif
        if (!drainMetadataFetches(app, std::chrono::seconds{5})) {
            std::cerr << "bookmarks-server: metadata-fetch dispatches did not settle within 5s; "
                         "shutting down anyway\n";
        }
    }

    std::cout << "bookmarks-server: stopped\n";
    return exitCode;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (std::exception const& error) {
        std::cerr << "bookmarks-server: " << error.what() << '\n';
        return 1;
    }
}

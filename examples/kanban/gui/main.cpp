// SPDX-License-Identifier: Apache-2.0

/// @file
/// kanban's desktop client shell: one `AppContext` (deployment mode from
/// `--server`), the two QML adapters `gui_lib/project_admin_qml_bridge.hpp`/
/// `gui_lib/board_qml_bridge.hpp` define built inside `ctx.onReady()`, and a
/// `QQmlApplicationEngine` loading this rung's own QML module (`Kanban`, see
/// `cmake/morph_add_rung.cmake`). Mirrors
/// `examples/bookmarks/gui/main.cpp`'s bootstrap pattern exactly — see that
/// file's own comments for the full rationale behind each step; only the
/// bridges and the authorizer/token-issuer types differ.
///
/// Usage:
/// @code
/// ladder_kanban_gui                                # in-process backend
/// ladder_kanban_gui --server ws://127.0.0.1:8768   # standalone server
/// ladder_kanban_gui --server ws://127.0.0.1:8768 --attachment-server http://127.0.0.1:8769
/// @endcode
///
/// `--attachment-server <url>` (Task 18) tells `BoardBridge` where Task 17's
/// `AttachmentServer` listens, so `uploadAttachment()`/`downloadAttachment()`
/// have somewhere to send their `QNetworkAccessManager` requests --
/// mirroring `--server`'s own convention for the WebSocket URL, since no
/// other configuration mechanism for this address exists anywhere in this
/// rung yet. Defaults to `http://127.0.0.1:8769` when `--server` is given
/// but `--attachment-server` is not -- the same default port
/// `src/server/main.cpp`'s own `KANBAN_ATTACHMENT_PORT` falls back to, so the
/// common case ("run both binaries with their own defaults") needs no flag
/// at all. Left unset entirely in Local (in-process) mode: that deployment
/// runs no `AttachmentServer` of its own (`kanban::app::App`, the durable
/// action log and real `KanbanAuthorizer`, lives only in the server binary
/// -- see the Local-mode comment below), so attachment upload/download
/// simply is not available there; `uploadAttachment()`/`downloadAttachment()`
/// report `failed()` rather than guessing an address nothing is listening
/// on.
///
/// Everything below the deployment-mode choice is intended to be shared
/// verbatim with a future `gui_wasm/main_wasm.cpp` — the adapters, and the
/// QML module all live outside this file precisely so the two clients can be
/// one program with two `main()`s (`examples/TESTING.md`, "same client
/// code").

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariant>
#include <cstdlib>
#include <memory>
#include <morph/session/session_auth.hpp>
#include <optional>
#include <string>

#include "board_qml_bridge.hpp"
#include "gui/app_context.hpp"
#include "kanban/auth/kanban_authorizer.hpp"
#include "kanban/db/database.hpp"
#include "project_admin_qml_bridge.hpp"

namespace {

/// @brief `--server <url>` if present, otherwise no url (in-process mode).
/// @param args The application's argument list.
/// @return The parsed url, or `std::nullopt` for in-process mode.
[[nodiscard]] std::optional<QUrl> serverUrlFromArgs(const QStringList& args) {
    const auto index = args.indexOf(QStringLiteral("--server"));
    if (index < 0 || index + 1 >= args.size()) {
        return std::nullopt;
    }
    return QUrl{args.at(index + 1)};
}

/// @brief `--attachment-server <url>` if present. Same parsing shape as
///        `serverUrlFromArgs` -- see this file's own `@file` comment for why
///        this flag exists and its default.
/// @param args The application's argument list.
/// @return The parsed url, or `std::nullopt` if the flag was not given.
[[nodiscard]] std::optional<QString> attachmentServerUrlFromArgs(const QStringList& args) {
    const auto index = args.indexOf(QStringLiteral("--attachment-server"));
    if (index < 0 || index + 1 >= args.size()) {
        return std::nullopt;
    }
    return args.at(index + 1);
}

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication qtApp{argc, argv};

    const auto serverUrl = serverUrlFromArgs(QCoreApplication::arguments());
    const auto attachmentServerUrl = attachmentServerUrlFromArgs(QCoreApplication::arguments());

    // Local mode hosts every model in this very process, so this process is
    // also the one that has to point Lightweight at a database, apply the
    // migrations, and install the `TokenIssuer` `Login` mints from — the
    // same bootstrap `src/server/main.cpp` performs, for the same reasons.
    // `Remote` mode must *not* do any of it: the server owns the store and
    // the signing secret, and a client opening the same SQLite file behind
    // the server's back is a second writer.
    //
    // Local mode is deliberately the *smaller* deployment, not an equivalent
    // one, exactly as bookmarks/polls/pastebin: `kanban::app::App` (the
    // durable action log and the real `KanbanAuthorizer`) lives only in the
    // server binary. A Local-mode client therefore journals nothing and —
    // because `LocalBackend` runs no authorizer at all — is authenticated
    // only in the sense that each model re-reads `session::current()->
    // principal` and scopes its own queries to it (`docs/spec/security.md`).
    // It is a single-user developer convenience; the multi-user isolation
    // this rung is *about* is only meaningful against the server.
    //
    // The Local-mode secret is a fixed literal on purpose: it is used to sign
    // and immediately verify a token inside one process that also owns the
    // database file, so it protects nothing and pretending otherwise (an env
    // var, a keyring) would suggest it does.
    if (!serverUrl) {
        const char* connectionString = std::getenv("KANBAN_DB");
        kanban::db::setup(connectionString != nullptr ? connectionString
                                                      : "DRIVER=SQLite3;Database=kanban.db;Timeout=5000");
        // hmacSha256 named explicitly -- see the identical note at
        // kanban/src/app/app.cpp's setTokenIssuer() call: TokenIssuer's
        // default is dropped entirely under MORPH_REQUIRE_VETTED_HMAC.
        kanban::auth::setTokenIssuer(std::make_shared<::morph::session::TokenIssuer>(
            std::string{"local-mode-development-secret"}, ::morph::session::hmacSha256));
    }

    // Mirrors AppContext's own doc-comment construction pattern: pick the
    // mode, then build every handler from inside onReady() — a Remote context
    // is *not* usable the line after its constructor returns (AppContext's
    // readiness contract, gui/app_context.hpp): a registration issued before
    // the socket connects is queued, and the handler it belongs to stays
    // unbound until the register reply lands
    // (`docs/spec/core/backend.md`, "Asynchronous registration").
    ::morph::ladder::gui::AppContext ctx{
        serverUrl ? ::morph::ladder::gui::AppContext::Mode{::morph::ladder::gui::Remote{.url = *serverUrl}}
                  : ::morph::ladder::gui::AppContext::Mode{::morph::ladder::gui::Local{.workers = 4}}};

    QQmlApplicationEngine engine;
    std::unique_ptr<kanban::gui::ProjectAdminBridge> projectAdminBridge;
    std::unique_ptr<kanban::gui::BoardBridge> boardBridge;

    ctx.onReady([&] {
        // Both adapters — and therefore all three `BridgeHandler`s they own
        // between them — are built here, once, and live until the process
        // exits. Nothing is torn down and rebuilt around login: login only
        // installs a session on the shared `Bridge`. See bookmarks' own
        // `main.cpp` for the full rationale (identical shape here).
        projectAdminBridge = std::make_unique<kanban::gui::ProjectAdminBridge>(ctx.bridge(), ctx.executor());
        boardBridge = std::make_unique<kanban::gui::BoardBridge>(ctx.bridge(), ctx.executor());
        // See this file's own @file comment for why this is unset entirely in
        // Local mode (no AttachmentServer to point at) and defaults to
        // src/server/main.cpp's own KANBAN_ATTACHMENT_PORT default otherwise.
        if (serverUrl) {
            boardBridge->setAttachmentServerUrl(attachmentServerUrl ? *attachmentServerUrl
                                                                    : QStringLiteral("http://127.0.0.1:8769"));
        }
#ifdef MORPH_BUILD_OFFLINE_SQLITE
        // Turns on BoardBridge's offline queue/replay stack (Task 5,
        // docs/superpowers/specs/2026-08-17-kanban-gui-design.md's now-updated
        // §1/§11): a dragged-task move made while offline queues into this
        // SQLite file instead of failing, and replays once NetworkMonitor's
        // own connectivity probe reports the backend reachable again.
        //
        // Uses `enableOfflineQueue`'s default (always-online) probe: this
        // rung has no dedicated "ping" action yet (see that method's own doc
        // comment), so the monitor never actually transitions offline in this
        // desktop client today, and moveTask() always takes its online path.
        // A real connectivity probe (e.g. a lightweight periodic no-op
        // action, once one exists) is a follow-up; this wiring proves the
        // queue/replay mechanism itself works end-to-end (this task's DoD),
        // not a production offline detector.
        //
        // IMPORTANT for that follow-up: enableOfflineQueue() posts
        // onOnline()/onOffline() onto ctx.executor() -- a QtExecutor
        // delivering onto *this* Qt GUI thread, not a background worker.
        // Harmless only because today's probe/tryReconnect always succeeds
        // immediately. A real, retry-capable tryReconnect wired here without
        // first moving onOnline()/onOffline() to a genuine background
        // executor would freeze the GUI thread for up to
        // maxAttempts * retryDelay (~20s at defaults) -- see
        // enableOfflineQueue()'s own doc comment and docs/spec/offline/
        // offline.md's "NetworkMonitor callback constraint".
        const char* offlineQueuePath = std::getenv("KANBAN_OFFLINE_QUEUE_DB");
        boardBridge->enableOfflineQueue(
            QString::fromUtf8(offlineQueuePath != nullptr ? offlineQueuePath : "kanban-offline-queue.db"));
#endif
        // Initial properties rather than context properties: the root object
        // then declares what it needs, so the same Main.qml also loads with
        // nothing wired up — which is exactly what the offscreen engine-load
        // smoke test (tests/test_gui_qml_smoke.cpp) does.
        engine.setInitialProperties({
            {QStringLiteral("projectAdminBridge"), QVariant::fromValue(projectAdminBridge.get())},
            {QStringLiteral("boardBridge"), QVariant::fromValue(boardBridge.get())},
        });
        engine.loadFromModule(MORPH_LADDER_QML_URI, "Main");
        if (engine.rootObjects().isEmpty()) {
            qWarning("ladder_kanban_gui: QML engine produced no root object");
            QCoreApplication::exit(1);
        }
    });

    if (serverUrl) {
        qInfo("ladder_kanban_gui: connecting to %s ...", qUtf8Printable(serverUrl->toString()));
    }
    return QGuiApplication::exec();
}

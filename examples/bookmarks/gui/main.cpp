// SPDX-License-Identifier: Apache-2.0

/// @file
/// bookmarks' desktop client shell: one `AppContext` (deployment mode from
/// `--server`), the four QML adapters `gui_lib/bookmark_qml_bridges.hpp`
/// defines built inside `ctx.onReady()`, and a `QQmlApplicationEngine`
/// loading this rung's own QML module (`Bookmarks`, see
/// `cmake/morph_add_rung.cmake`).
///
/// Usage:
/// @code
/// ladder_bookmarks_gui                                # in-process backend
/// ladder_bookmarks_gui --server ws://127.0.0.1:8766   # standalone server
/// @endcode
///
/// Everything below the deployment-mode choice is intended to be shared
/// verbatim with a future `gui_wasm/main_wasm.cpp` — the adapters, the schema
/// document and the QML module all live outside this file precisely so the
/// two clients can be one program with two `main()`s (`examples/TESTING.md`,
/// "same client code").

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

#include "bookmark_qml_bridges.hpp"
#include "bookmarks/auth/bookmarks_authorizer.hpp"
#include "bookmarks/db/database.hpp"
#include "gui/app_context.hpp"

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

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication qtApp{argc, argv};

    const auto serverUrl = serverUrlFromArgs(QCoreApplication::arguments());

    // Local mode hosts every model in this very process, so this process is
    // also the one that has to point Lightweight at a database, apply the
    // migrations, and install the `TokenIssuer` `AuthModel` mints from —
    // the same bootstrap `src/server/main.cpp` performs, for the same
    // reasons. `Remote` mode must *not* do any of it: the server owns the
    // store and the signing secret, and a client opening the same SQLite file
    // behind the server's back is a second writer.
    //
    // Local mode is deliberately the *smaller* deployment, not an equivalent
    // one, exactly as in rung 1: `bookmarks::app::App` (the durable action
    // log, the metadata-fetch worker, the outbox relay and the real
    // `BookmarksAuthorizer`) lives only in the server binary. A Local-mode
    // client therefore journals nothing, never fetches a title, never relays
    // an outbox row, and — because `LocalBackend` runs no authorizer at all —
    // is authenticated only in the sense that each model re-reads
    // `session::current()->principal` and scopes its own queries to it
    // (`docs/spec/security.md`; `examples/bookmarks/README.md`'s "Local mode
    // has no authorization at all" strain point). It is a single-user
    // developer convenience; the two-user isolation this rung is *about* is
    // only meaningful against the server.
    //
    // The Local-mode secret is a fixed literal on purpose: it is used to sign
    // and immediately verify a token inside one process that also owns the
    // database file, so it protects nothing and pretending otherwise (an
    // env var, a keyring) would suggest it does.
    if (!serverUrl) {
        const char* connectionString = std::getenv("BOOKMARKS_DB");
        bookmarks::db::setup(connectionString != nullptr ? connectionString
                                                         : "DRIVER=SQLite3;Database=bookmarks.db;Timeout=5000");
        // hmacSha256 named explicitly -- see the identical note at
        // bookmarks/src/app/app.cpp's setTokenIssuer() call: TokenIssuer's
        // default is dropped entirely under MORPH_REQUIRE_VETTED_HMAC.
        bookmarks::auth::setTokenIssuer(std::make_shared<::morph::session::TokenIssuer>(
            std::string{"local-mode-development-secret"}, ::morph::session::hmacSha256));
    }

    // Mirrors AppContext's own doc-comment construction pattern: pick the
    // mode, then build every handler from inside onReady(). `Remote` mode
    // builds its backend with `asyncRegistrationEnabled`, and
    // `QtWebSocketBackend::registerModelAsync()` queues a registration issued
    // before the socket finishes connecting and retries it once the connection
    // comes up (`docs/spec/core/backend.md`, "Asynchronous registration"), so
    // this ordering is no longer load-bearing for correctness — it is simply
    // the one shape that reads the same in both modes (`Local` is ready on
    // construction and runs onReady() inline). See
    // `examples/common/gui/app_context.hpp`'s "Readiness contract" and
    // `examples/TESTING.md` presenter rule 2.
    ::morph::ladder::gui::AppContext ctx{
        serverUrl ? ::morph::ladder::gui::AppContext::Mode{::morph::ladder::gui::Remote{.url = *serverUrl}}
                  : ::morph::ladder::gui::AppContext::Mode{::morph::ladder::gui::Local{.workers = 4}}};

    QQmlApplicationEngine engine;
    std::unique_ptr<bookmarks::gui::FormsBridge> formsBridge;
    std::unique_ptr<bookmarks::gui::BookmarkBridge> bookmarkBridge;
    std::unique_ptr<bookmarks::gui::TagBridge> tagBridge;
    std::unique_ptr<bookmarks::gui::SharedFeedBridge> feedBridge;

    ctx.onReady([&] {
        // All four adapters — and therefore all six `BridgeHandler`s they own
        // between them — are built here, once, and live until the process
        // exits. Nothing is torn down and rebuilt around login: login only
        // installs a session on the shared `Bridge`. That is deliberate:
        // `QtWebSocketBackend::deregisterModel` now assigns its fire-and-
        // forget `deregister` envelope a real, tracked callId rather than
        // sharing the `callId == 0` sentinel a subsequent synchronous
        // register/attach/assign call also used to, closing a race that used
        // to be able to permanently zero a freshly constructed handler's
        // model id if it was built on the same connection right after an
        // older one was torn down — but this shape (build once, never rebuild)
        // was never the shape that race needed in the first place, so it
        // stays regardless.
        formsBridge = std::make_unique<bookmarks::gui::FormsBridge>(ctx.bridge(), ctx.executor());
        bookmarkBridge = std::make_unique<bookmarks::gui::BookmarkBridge>(ctx.bridge(), ctx.executor());
        tagBridge = std::make_unique<bookmarks::gui::TagBridge>(ctx.bridge(), ctx.executor());
        feedBridge = std::make_unique<bookmarks::gui::SharedFeedBridge>(ctx.bridge(), ctx.executor());
        // Initial properties rather than context properties: the root object
        // then declares what it needs, so the same Main.qml also loads with
        // nothing wired up — which is exactly what the offscreen engine-load
        // smoke test (tests/test_gui_qml_smoke.cpp) does.
        engine.setInitialProperties({
            {QStringLiteral("formsController"), QVariant::fromValue(formsBridge.get())},
            {QStringLiteral("bookmarkController"), QVariant::fromValue(bookmarkBridge.get())},
            {QStringLiteral("tagController"), QVariant::fromValue(tagBridge.get())},
            {QStringLiteral("feedController"), QVariant::fromValue(feedBridge.get())},
        });
        engine.loadFromModule(MORPH_LADDER_QML_URI, "Main");
        if (engine.rootObjects().isEmpty()) {
            qWarning("ladder_bookmarks_gui: QML engine produced no root object");
            QCoreApplication::exit(1);
        }
    });

    if (serverUrl) {
        qInfo("ladder_bookmarks_gui: connecting to %s ...", qUtf8Printable(serverUrl->toString()));
    }
    return QGuiApplication::exec();
}

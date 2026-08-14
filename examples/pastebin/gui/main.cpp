// SPDX-License-Identifier: Apache-2.0

/// @file
/// pastebin's desktop client shell: one `AppContext` (deployment mode from
/// `--server`), the two QML adapters `gui_lib/paste_qml_bridges.hpp` defines
/// built inside `ctx.onReady()`, and a `QQmlApplicationEngine` loading this
/// rung's own QML module (`Pastebin`, see `cmake/morph_add_rung.cmake`).
///
/// Usage:
/// @code
/// ladder_pastebin_gui                             # in-process backend
/// ladder_pastebin_gui --server ws://127.0.0.1:8765  # standalone server
/// @endcode
///
/// Everything below the deployment-mode choice is shared verbatim with
/// `gui_wasm/main_wasm.cpp` — the adapters, the schema document and the QML
/// module all live outside this file precisely so the two clients are one
/// program with two `main()`s (`examples/TESTING.md`, "same client code").

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariant>

#include "gui/app_context.hpp"
#include "paste_qml_bridges.hpp"
#include "pastebin/db/database.hpp"

#include <cstdlib>
#include <memory>
#include <optional>

namespace {

/// @brief `--server <url>` if present, otherwise no url (in-process mode).
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

    // Local mode hosts `PasteModel` in this very process, so this process is
    // also the one that has to point Lightweight at a database and apply the
    // migrations — the same bootstrap `src/server/main.cpp` performs, for the
    // same reason. `Remote` mode must *not* do it: the server owns the store,
    // and a client opening the same SQLite file behind the server's back is
    // exactly the second writer this rung's SQLITE_BUSY work exists to avoid.
    //
    // Local mode is deliberately the *smaller* deployment, not an equivalent
    // one: `pastebin::app::App` (the durable action log and the periodic
    // expiry sweep) lives only in the server binary. A Local-mode client
    // therefore journals nothing, and an expired paste keeps appearing in the
    // listing until something sweeps it — `ListPastes` filters on visibility
    // only, and it is `ExpirePaste` that reclaims the row
    // (`src/models/paste_model.cpp`). Opening one still fails correctly with
    // "paste has expired", because `GetPaste`'s own atomic guard never depends
    // on the sweep having run.
    if (!serverUrl) {
        const char* connectionString = std::getenv("PASTEBIN_DB");
        pastebin::db::setup(connectionString != nullptr ? connectionString
                                                        : "DRIVER=SQLite3;Database=pastebin.db;Timeout=5000");
    }

    // Mirrors AppContext's own doc-comment construction pattern: pick the
    // mode, then build every handler from inside onReady() — a Remote context
    // is *not* usable the line after its constructor returns
    // (docs/findings/017).
    ::morph::ladder::gui::AppContext ctx{
        serverUrl ? ::morph::ladder::gui::AppContext::Mode{::morph::ladder::gui::Remote{.url = *serverUrl}}
                  : ::morph::ladder::gui::AppContext::Mode{::morph::ladder::gui::Local{.workers = 4}}};

    QQmlApplicationEngine engine;
    std::unique_ptr<pastebin::gui::FormsBridge> formsBridge;
    std::unique_ptr<pastebin::gui::PasteBridge> pasteBridge;

    ctx.onReady([&] {
        formsBridge = std::make_unique<pastebin::gui::FormsBridge>(ctx.bridge(), ctx.executor());
        pasteBridge = std::make_unique<pastebin::gui::PasteBridge>(ctx.bridge(), ctx.executor());
        // Initial properties rather than context properties: the root object
        // then declares what it needs, so the same Main.qml also loads with
        // nothing wired up — which is exactly what the offscreen engine-load
        // smoke test (tests/test_gui_qml_smoke.cpp) does.
        engine.setInitialProperties({
            {QStringLiteral("formsController"), QVariant::fromValue(formsBridge.get())},
            {QStringLiteral("pasteController"), QVariant::fromValue(pasteBridge.get())},
        });
        engine.loadFromModule(MORPH_LADDER_QML_URI, "Main");
        if (engine.rootObjects().isEmpty()) {
            qWarning("ladder_pastebin_gui: QML engine produced no root object");
            QCoreApplication::exit(1);
        }
    });

    if (serverUrl) {
        qInfo("ladder_pastebin_gui: connecting to %s ...", qUtf8Printable(serverUrl->toString()));
    }
    return QGuiApplication::exec();
}

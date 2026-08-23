// SPDX-License-Identifier: Apache-2.0

/// @file
/// lims' desktop client shell: one `AppContext` (deployment mode from
/// `--server`), the two QML adapters `gui_lib/sample_qml_bridge.hpp` and
/// `gui_lib/result_qml_bridge.hpp` define, both built inside `ctx.onReady()`,
/// and a `QQmlApplicationEngine` loading this rung's own QML module (`Lims`,
/// see `cmake/morph_add_rung.cmake`). Mirrors
/// `examples/kanban/gui/main.cpp`'s bootstrap pattern — see that file's own
/// comments for the full rationale behind each step.
///
/// Usage:
/// @code
/// ladder_lims_gui                                # in-process backend
/// ladder_lims_gui --server ws://127.0.0.1:8770   # standalone server
/// @endcode
///
/// Everything below the deployment-mode choice is intended to be shared
/// verbatim with a future `gui_wasm/main_wasm.cpp` — the adapters and the QML
/// module all live outside this file precisely so the two clients can be one
/// program with two `main()`s (`examples/TESTING.md`, "same client code").
///
/// @par What Local mode is, and is not
/// Local mode hosts every model in this process, so this process is the one
/// that points Lightweight at a database and applies the migrations. It runs
/// no `RemoteServer`, therefore no `IAuthorizer`, therefore none of
/// `lims::auth::LimsAuthorizer`'s edge role gate. That is not a hole: every
/// role check this rung actually depends on is re-checked inside the model on
/// every dispatch path (the rung README's §6 decision), so a Local-mode
/// client is gated exactly as hard as a remote one — it simply gets one layer
/// instead of two. It journals nothing, though, because the durable action
/// log lives in the server bootstrap.

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariant>

#include "gui/app_context.hpp"
#include "lims/db/database.hpp"
#include "result_qml_bridge.hpp"
#include "sample_qml_bridge.hpp"

#include <cstdlib>
#include <memory>
#include <optional>

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

    // Remote mode must not do this: the server owns the store, and a client
    // opening the same SQLite file behind its back is a second writer.
    if (!serverUrl) {
        const char* connectionString = std::getenv("LIMS_DB");
        lims::db::setup(connectionString != nullptr ? connectionString
                                                    : "DRIVER=SQLite3;Database=lims.db;Timeout=5000");
    }

    ::morph::ladder::gui::AppContext ctx{
        serverUrl ? ::morph::ladder::gui::AppContext::Mode{::morph::ladder::gui::Remote{.url = *serverUrl}}
                  : ::morph::ladder::gui::AppContext::Mode{::morph::ladder::gui::Local{.workers = 4}}};

    QQmlApplicationEngine engine;
    std::unique_ptr<lims::gui::SampleBridge> sampleBridge;
    std::unique_ptr<lims::gui::ResultBridge> resultBridge;

    ctx.onReady([&] {
        // Both adapters — and therefore all three `BridgeHandler`s they own
        // between them — are built here, once, and live until the process
        // exits. A `Remote` context is not usable the line after its
        // constructor returns, which is what `onReady` is for.
        sampleBridge = std::make_unique<lims::gui::SampleBridge>(ctx.bridge(), ctx.executor());
        resultBridge = std::make_unique<lims::gui::ResultBridge>(ctx.bridge(), ctx.executor());

        // Initial properties rather than context properties: the root object
        // then declares what it needs, so the same Main.qml also loads with
        // nothing wired up — which is exactly what the offscreen engine-load
        // smoke test (tests/test_gui_qml_smoke.cpp) does.
        engine.setInitialProperties({
            {QStringLiteral("sampleBridge"), QVariant::fromValue(sampleBridge.get())},
            {QStringLiteral("resultBridge"), QVariant::fromValue(resultBridge.get())},
        });
        engine.loadFromModule(MORPH_LADDER_QML_URI, "Main");
        if (engine.rootObjects().isEmpty()) {
            qWarning("ladder_lims_gui: QML engine produced no root object");
            QCoreApplication::exit(1);
        }
    });

    if (serverUrl) {
        qInfo("ladder_lims_gui: connecting to %s ...", qUtf8Printable(serverUrl->toString()));
    }
    return QGuiApplication::exec();
}

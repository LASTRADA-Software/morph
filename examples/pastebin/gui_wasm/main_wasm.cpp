// SPDX-License-Identifier: Apache-2.0

/// @file
/// pastebin's WebAssembly client shell — rung 1's payoff on rung 0's
/// WASM-remote spike (`examples/common/wasm_spike/`).
///
/// This file is the *only* difference between the browser client and the
/// desktop client (`gui/main.cpp`). Everything with behaviour in it — the
/// presenters (`gui_lib/paste_presenter.hpp`), the forms controller
/// (`gui_lib/paste_forms_controller.hpp`), the QML adapters
/// (`gui_lib/paste_qml_bridges.hpp`), the schema document
/// (`gui_lib/paste_schemas.hpp`) and the QML itself (`gui/qml/Main.qml`, built
/// into the `Pastebin` module both binaries link) — is shared verbatim. That
/// is `examples/TESTING.md`'s "same client code" requirement, and its explicit
/// ban on bank's `gui_wasm` shadow-header pattern: no model, DTO, presenter or
/// QML file has a WASM variant here.
///
/// Two things are genuinely WASM-specific, and both are one line each:
///
/// * **Mode.** There is no `--server` flag and no `Local` alternative. A
///   browser has no ODBC and no in-process server to be `Local` against, so a
///   ladder WASM client is always `Remote` (`examples/IMPLEMENTATION.md` rule
///   4's WASM clause: "Lightweight (ODBC) cannot run in the browser… the
///   ladder's WASM clients are **remote clients** — persistence lives
///   server-side, behind the model"). The url is baked in at build time via
///   `MORPH_LADDER_PASTEBIN_WASM_SERVER_URL` (`../CMakeLists.txt`), following
///   the spike's own `MORPH_LADDER_WASM_SPIKE_SERVER_URL` convention — a page
///   served from a static bundle has no argv to read one from.
/// * **No database bootstrap.** `gui/main.cpp` calls `pastebin::db::setup()`
///   in `Local` mode; there is nothing to set up here.
///
/// Note what is *not* here: no `asyncRegistrationEnabled` flag, no
/// `setConnectHandler`, no hand-rolled wait-for-binding timer. The spike had
/// to hand-roll all three; `AppContext` (`examples/common/gui/app_context.hpp`)
/// now owns the first two generically for every client, native or browser, and
/// `Main.qml`'s bootstrap-retry `Timer` — shared, like the rest of the QML —
/// covers the third (`docs/findings/024`, the "handler not bound" window that
/// opens on connect and closes when registration settles; it is a *remote*
/// mode gap, so this client hits exactly the same one the desktop client does
/// in `--server` mode, and is covered by exactly the same mitigation).
///
/// @par Verification status
/// Structurally complete and reviewed, **never compiled**: no Emscripten
/// toolchain was available in the environment this was authored in, exactly as
/// `examples/common/wasm_spike/README.md` records for the spike. The
/// `ladder-wasm` compile gate added to `.github/workflows/wasm-ladder.yml` is
/// what will actually prove it, on the first push that runs it.

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QString>
#include <QUrl>
#include <QVariant>

#include "gui/app_context.hpp"
#include "paste_qml_bridges.hpp"

#include <memory>

int main(int argc, char** argv) {
    QGuiApplication qtApp{argc, argv};

    // Always Remote — see this file's header comment. `AppContext` builds the
    // QtWebSocketBackend with asyncRegistrationEnabled=true, which is what
    // makes registration WASM-safe at all (the synchronous path nests a
    // QEventLoop and aborts the page — examples/TESTING.md, "WASM reality").
    ::morph::ladder::gui::AppContext ctx{::morph::ladder::gui::Remote{
        .url = QUrl{QString::fromUtf8(MORPH_LADDER_PASTEBIN_WASM_SERVER_URL)}}};

    QQmlApplicationEngine engine;
    std::unique_ptr<pastebin::gui::FormsBridge> formsBridge;
    std::unique_ptr<pastebin::gui::PasteBridge> pasteBridge;

    // Every handler is built from inside onReady(), never before it: a Remote
    // context is not usable the line after its constructor returns, and a
    // registration issued before the socket is up fails permanently with no
    // retry (docs/findings/017). Identical to gui/main.cpp's --server path.
    ctx.onReady([&] {
        formsBridge = std::make_unique<pastebin::gui::FormsBridge>(ctx.bridge(), ctx.executor());
        pasteBridge = std::make_unique<pastebin::gui::PasteBridge>(ctx.bridge(), ctx.executor());
        engine.setInitialProperties({
            {QStringLiteral("formsController"), QVariant::fromValue(formsBridge.get())},
            {QStringLiteral("pasteController"), QVariant::fromValue(pasteBridge.get())},
        });
        engine.loadFromModule(MORPH_LADDER_QML_URI, "Main");
        if (engine.rootObjects().isEmpty()) {
            qWarning("ladder_pastebin_gui_wasm: QML engine produced no root object");
        }
    });

    qInfo("ladder_pastebin_gui_wasm: connecting to %s ...", MORPH_LADDER_PASTEBIN_WASM_SERVER_URL);
    return QGuiApplication::exec();
}

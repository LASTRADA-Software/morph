// SPDX-License-Identifier: Apache-2.0

/// @file
/// bookmarks' WebAssembly client shell — rung 2's counterpart to rung 1's
/// `examples/pastebin/gui_wasm/main_wasm.cpp`, mirrored from it exactly.
///
/// This file is the *only* difference between the browser client and the
/// desktop client (`gui/main.cpp`). Everything with behaviour in it — the
/// presenters (`gui_lib/bookmark_presenter.hpp`, `gui_lib/tag_presenter.hpp`,
/// `gui_lib/shared_feed_presenter.hpp`), the forms controller
/// (`gui_lib/bookmark_forms_controller.hpp`), the QML adapters
/// (`gui_lib/bookmark_qml_bridges.hpp`), the schema document
/// (`gui_lib/bookmark_schemas.hpp`) and the QML itself (`gui/qml/Main.qml`,
/// built into the `Bookmarks` module both binaries link) — is shared
/// verbatim. That is `examples/TESTING.md`'s "same client code" requirement,
/// and its explicit ban on bank's `gui_wasm` shadow-header pattern: no model,
/// DTO, presenter or QML file has a WASM variant here.
///
/// Two things are genuinely WASM-specific, and both are one line each:
///
/// * **Mode.** There is no `--server` flag and no `Local` alternative. A
///   browser has no ODBC and no in-process server to be `Local` against, so a
///   ladder WASM client is always `Remote` (`examples/IMPLEMENTATION.md` rule
///   4's WASM clause: "Lightweight (ODBC) cannot run in the browser… the
///   ladder's WASM clients are **remote clients** — persistence lives
///   server-side, behind the model"). The url is baked in at build time via
///   `MORPH_LADDER_BOOKMARKS_WASM_SERVER_URL` (`../CMakeLists.txt`), following
///   pastebin's own `MORPH_LADDER_PASTEBIN_WASM_SERVER_URL` convention — a
///   page served from a static bundle has no argv to read one from.
/// * **No database bootstrap, no local `TokenIssuer`.** `gui/main.cpp` calls
///   `bookmarks::db::setup()` and installs a dev-mode `TokenIssuer` only in
///   `Local` mode (`if (!serverUrl)`); there is nothing to set up here — the
///   server owns the store and the signing secret, and login mints a real
///   token over the wire via `AuthModel`/`FormsBridge`, exactly as the
///   desktop client's own `--server` path does.
///
/// Note what is *not* here: no `asyncRegistrationEnabled` flag, no
/// `setConnectHandler`, no hand-rolled wait-for-binding timer. The
/// `examples/common/wasm_spike/main_wasm.cpp` spike had to hand-roll both;
/// `AppContext` (`examples/common/gui/app_context.hpp`) now owns them
/// generically for every client, native or browser. This rung hits the same
/// "handler not bound" window pastebin's own `--server`/WASM clients do (the
/// registration round trip that opens on connect and closes once it lands),
/// but needs no `whenBound()`-gated bootstrap dispatch of its own the way
/// `pastebin::gui::PasteBridge::bound` gates `Main.qml`'s first `refresh()`:
/// nothing in this rung's `Main.qml` dispatches on `Component.onCompleted`,
/// so the window closes before a user can click anything, not before a
/// bootstrap call needs to land.
///
/// @par Verification status
/// Structurally complete and reviewed, **never compiled**: no Emscripten
/// toolchain was available in the environment this was authored in, exactly
/// as rung 1's own `gui_wasm/main_wasm.cpp` and
/// `examples/common/wasm_spike/README.md` record. The `ladder-wasm` compile
/// gate in `.github/workflows/wasm-ladder.yml` is what will actually prove
/// it, on the first push that runs it.

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QString>
#include <QUrl>
#include <QVariant>

#include "bookmark_qml_bridges.hpp"
#include "gui/app_context.hpp"

#include <memory>

int main(int argc, char** argv) {
    QGuiApplication qtApp{argc, argv};

    // Always Remote — see this file's header comment. `AppContext` builds the
    // QtWebSocketBackend with asyncRegistrationEnabled=true, which is what
    // makes registration WASM-safe at all (the synchronous path nests a
    // QEventLoop and aborts the page — examples/TESTING.md, "WASM reality").
    ::morph::ladder::gui::AppContext ctx{::morph::ladder::gui::Remote{
        .url = QUrl{QString::fromUtf8(MORPH_LADDER_BOOKMARKS_WASM_SERVER_URL)}}};

    QQmlApplicationEngine engine;
    std::unique_ptr<bookmarks::gui::FormsBridge> formsBridge;
    std::unique_ptr<bookmarks::gui::BookmarkBridge> bookmarkBridge;
    std::unique_ptr<bookmarks::gui::TagBridge> tagBridge;
    std::unique_ptr<bookmarks::gui::SharedFeedBridge> feedBridge;

    // Every handler is built from inside onReady(), never before it -- a
    // Remote context is not usable the line after its constructor returns,
    // per AppContext's own readiness contract. Identical to gui/main.cpp's
    // --server path, including building all four adapters up front rather
    // than tearing one down and rebuilding it around login -- see that
    // file's identical comment for why (a since-fixed deregister-callId
    // race this shape was never actually exposed to anyway).
    ctx.onReady([&] {
        formsBridge = std::make_unique<bookmarks::gui::FormsBridge>(ctx.bridge(), ctx.executor());
        bookmarkBridge = std::make_unique<bookmarks::gui::BookmarkBridge>(ctx.bridge(), ctx.executor());
        tagBridge = std::make_unique<bookmarks::gui::TagBridge>(ctx.bridge(), ctx.executor());
        feedBridge = std::make_unique<bookmarks::gui::SharedFeedBridge>(ctx.bridge(), ctx.executor());
        engine.setInitialProperties({
            {QStringLiteral("formsController"), QVariant::fromValue(formsBridge.get())},
            {QStringLiteral("bookmarkController"), QVariant::fromValue(bookmarkBridge.get())},
            {QStringLiteral("tagController"), QVariant::fromValue(tagBridge.get())},
            {QStringLiteral("feedController"), QVariant::fromValue(feedBridge.get())},
        });
        engine.loadFromModule(MORPH_LADDER_QML_URI, "Main");
        if (engine.rootObjects().isEmpty()) {
            qWarning("ladder_bookmarks_gui_wasm: QML engine produced no root object");
        }
    });

    qInfo("ladder_bookmarks_gui_wasm: connecting to %s ...", MORPH_LADDER_BOOKMARKS_WASM_SERVER_URL);
    return QGuiApplication::exec();
}

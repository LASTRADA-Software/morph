// SPDX-License-Identifier: Apache-2.0

/// @file
/// polls' WebAssembly client shell — rung 3's counterpart to
/// `examples/bookmarks/gui_wasm/main_wasm.cpp` (rung 2) and
/// `examples/pastebin/gui_wasm/main_wasm.cpp` (rung 1), mirrored from
/// bookmarks' structurally, with three genuinely new things neither prior
/// rung's WASM client needed.
///
/// This file is the *only* difference between the browser client and a
/// desktop client. (This rung, as of this task, ships no
/// `examples/polls/gui/main.cpp` at all — no task in this plan wrote one —
/// so today this is in fact polls' *only* GUI client binary; see this file's
/// "Verification status" section below for what that implies.) Everything
/// with behaviour in it — `gui_lib/poll_presenter.hpp`,
/// `gui_lib/poll_forms_controller.hpp`, `gui_lib/poll_qml_bridges.hpp`,
/// `gui_lib/poll_schemas.hpp`, and the QML itself (`gui/qml/{Main,VoteView,
/// CreatePollView}.qml`, built into the `Polls` module) — is shared verbatim
/// with whatever desktop client a future task adds. That is
/// `examples/TESTING.md`'s "same client code" requirement, and its explicit
/// ban on bank's `gui_wasm` shadow-header pattern: no model, DTO, presenter
/// or QML file has a WASM variant here.
///
/// @par Mode and the WASM server url
/// Always `Remote` — a browser has no ODBC and no in-process server to be
/// `Local` against (`examples/IMPLEMENTATION.md` rule 4's WASM clause). The
/// url is baked in at build time via `MORPH_LADDER_POLLS_WASM_SERVER_URL`
/// (`../CMakeLists.txt`), following pastebin's/bookmarks' own convention — a
/// page served from a static bundle has no argv to read one from.
///
/// @par No database bootstrap, no `TokenIssuer` — same as every ladder rung's
/// WASM client, but for a slightly different reason here: this rung has
/// **no `TokenIssuer`/signed tokens at all**, native or WASM
/// (`examples/polls/README.md`'s Global Constraints, judgment call 2 — a
/// deliberate departure from rung 1/2's pattern, forced by there being no
/// framework authorizer for bare shared secrets). `CreatePoll` mints its
/// admin/participant tokens itself, inside `PollModel::execute()`; there is
/// no signing secret for this file to *not* set up, unlike pastebin's/
/// bookmarks' own "no bootstrap" note.
///
/// @par `nativeClient: false` — the only way `CreatePollView.qml` stays reachable-nowhere
/// `CreatePoll` is native-client-only (`examples/polls/README.md`'s Global
/// Constraints). `gui/qml/Main.qml`'s `ApplicationWindow` declares
/// `property bool nativeClient: true` for exactly this file to flip — its own
/// doc comment (written by Task 16, before this file existed) already
/// anticipates "a future gui_wasm/main_wasm.cpp is expected to pass
/// `nativeClient: false` as an initial property". Passed the same way as
/// `pollBridge` below, through `QQmlApplicationEngine::setInitialProperties`
/// (a root-object property set from C++ right after the engine is
/// constructed — the same mechanism bookmarks' own WASM client uses for its
/// controller properties, generalised here to a plain `bool`).
///
/// Verified, not assumed, that this actually makes `CreatePollView.qml`
/// unreachable: grepping `gui/qml/*.qml` for every reference to `createPage`/
/// `CreatePollView` turns up exactly one route to it — `Main.qml`'s landing
/// screen's "Create a new poll (organizer)" `Button`, whose `visible` is
/// `root.nativeClient` (not merely `enabled` — an invisible `Button` in Qt
/// Quick Controls receives no hit-testing at all, so this is not just a
/// dimmed affordance a determined user could still click). With
/// `nativeClient: false`, nothing in the shared QML ever calls
/// `stack.push(createPage)`; `CreatePollView.qml` itself is still linked into
/// the one shared `ladder_polls_qml` module both a future desktop client and
/// this binary would use (`examples/TESTING.md`'s "same client code" rule
/// bans a WASM-only QML variant that would omit it entirely), but a shipped
/// component that no code path ever instantiates is exactly as unreachable,
/// from a participant's perspective, as one that was never compiled in.
///
/// @par The pollId URL parameter — the participant's way in, without `CreatePoll`
/// A WASM participant needs a way to land on a specific poll's `VoteView`
/// without going through the native-only `CreatePollView`/organizer flow.
/// `gui/qml/Main.qml`'s landing screen already offers a manual `TextField` +
/// "Open" button for pasting a poll id by hand — that alone is enough to use
/// this client at all — but a shared poll *link* (`https://.../?poll=<id>`)
/// should skip that step. Neither `examples/common/wasm_spike/main_wasm.cpp`
/// (rung 0's WASM-remote spike) nor pastebin's/bookmarks' own WASM clients
/// establish any URL-parameter precedent — none of them takes anything from
/// the page url at all, both baking their server url in at *build* time
/// instead of reading anything at *run* time.
///
/// Researched two ways to read the browser url from a Qt-for-WebAssembly
/// binary before picking one:
///  - **Qt's documented-in-forums-only "URL query becomes argv" behaviour**
///    (`?arg1&arg2` turning into extra `QGuiApplication::arguments()`
///    entries) turns out to require either the `--emrun` Emscripten link
///    flag (this project's WASM targets do not pass it — `emrun` is a local
///    dev-server convenience, not something a static-bundle deploy uses) or
///    hand-patching the generated `qtloader.js`'s `Module.arguments` after
///    the fact, outside this repository's CMake entirely. Both are
///    build-configuration-shaped, not something `main_wasm.cpp` itself can
///    rely on, and neither is present in `doc.qt.io/qt-6/wasm.html`'s
///    current text — it looks like older/unofficial `qtloader.js` behaviour
///    that this project's build does not opt into.
///  - **Reading `window.location.search` directly**, via a small Emscripten
///    `EM_JS` shim, needs no such flags: `EM_JS`/`EM_ASM` code is inlined
///    directly into the generated JS module and always has access to the
///    runtime's internal helpers (`UTF8ToString`, `stringToUTF8`,
///    `lengthBytesUTF8`, `_malloc`) regardless of `EXPORTED_RUNTIME_METHODS`
///    — unlike calling into `Module.*` from *external* JS, which those
///    exports actually gate. This also avoids requiring Embind's `--bind`
///    (`emscripten::val` would need it; this target's CMake does not pass
///    it), so `pollsWasmQueryPollId()` below is the chosen mechanism —
///    established here as this repository's first precedent for reading the
///    browser url from a WASM QML client, for a future rung to reuse or
///    improve on.
///
/// The `poll` parameter is absent (empty string) whenever the page was
/// opened without one — the manual `TextField` path on the landing screen
/// still works identically in that case; `Main.qml`'s new `initialPollId`
/// property (added by this task, empty by default, so every prior QML smoke
/// test's assertions are unaffected) is a no-op unless this file passes it a
/// non-empty value.
///
/// @par Note what is *not* here, and why this is the first WASM binary that can say so honestly
/// No `asyncRegistrationEnabled` flag, no `setConnectHandler`, no
/// hand-rolled wait-for-binding timer — `AppContext`
/// (`examples/common/gui/app_context.hpp`) owns the first two generically,
/// confirmed still true by reading `examples/common/gui/app_context.cpp:37`,
/// which builds this client's `QtWebSocketBackend` with
/// `Config{.asyncRegistrationEnabled = true}` for every ladder GUI/WASM app,
/// polls included, unconditionally. No new wiring was needed here beyond
/// what `AppContext` already provides.
///
/// More interestingly: this is also the first ladder WASM client with
/// *no hand-rolled retry timer anywhere in its QML*, and that is not an
/// oversight — `gui/qml/VoteView.qml`'s `Component.onCompleted` fires
/// `pollBridge.openPoll(pollId)` exactly once, unconditionally, with nothing
/// resembling pastebin's `Main.qml`/bookmarks' `BookmarkListView.qml`
/// bootstrap-retry `Timer` (both covering docs/findings/024, "the handler
/// not bound window that opens on connect and closes when registration
/// settles"). Read `include/morph/core/bridge.hpp` to confirm this is
/// actually safe rather than assuming this rung's `EventPoller` quietly
/// papers over a real gap:
///  - Pastebin's/bookmarks' plain (`NoSharing`) handlers each call
///    `Bridge::registerHandler(binding)` at construction, which — via
///    `registerHandlerImpl` — issues a real `registerModelAsync` round trip
///    to the backend. Until that reply lands, `binding->currentId` stays `0`
///    and any call through the handler fails "handler not bound"; that
///    window is exactly finding 024, and why those two rungs' `Main.qml`
///    equivalents retry the first dispatch on a short timer.
///  - `PollFormsController`'s handler (`BridgeHandler<PollModel,
///    AllowShared>`) is built via `Bridge::registerSharedHandler<Model>()`
///    instead (`bridge.hpp`'s `BridgeHandler::makeBinding`, `kShared`
///    branch), whose own doc comment says plainly: "this registers nothing
///    on the backend: a shared handler has no instance until a keyed action
///    ... tells it which one it wants." There is no preliminary round trip
///    to race at all. The handler's first, and only, network operation is
///    `Bridge::attachHandlerAsync` itself, fired directly from
///    `PollFormsController::openPoll()` — which `VoteView.qml`'s
///    `Component.onCompleted` only ever calls after `PollBridge` has been
///    constructed, which this file only ever does from inside
///    `ctx.onReady()` (below), by which point the socket is already
///    connected (finding 017's window is closed) and there is no *second*,
///    separate registration step left to still be pending (finding 024's
///    window never opens in the first place). This is the exact keyed-attach
///    async path `docs/superpowers/plans/2026-08-07-ladder-rung3-framework-prereqs.md`
///    closed finding 032 for, and this file is the first real WASM binary
///    to actually dispatch through it.
///
/// @par Verification status
/// Structurally complete and reviewed, **never compiled**: no Emscripten
/// toolchain was available in the environment this was authored in, exactly
/// as rung 0's spike, rung 1's and rung 2's own `gui_wasm/main_wasm.cpp`
/// record for themselves. This file carries strictly more unverified surface
/// than either of those: the `pollsWasmQueryPollId()` `EM_JS` shim below is
/// this repository's first use of `EM_JS`/raw Emscripten JS interop anywhere
/// (previously only Qt's own WASM platform layer touched JS at all), and the
/// keyed-attach dispatch path it feeds (`OpenPoll` → `attachHandlerAsync`)
/// has, per the reasoning above, literally never run inside a real WASM
/// binary before. The `ladder-wasm` compile gate in
/// `.github/workflows/wasm-ladder.yml` (which this task extends with a named
/// `ladder_polls_gui_wasm` target) is what will actually prove the compile
/// half; nothing short of a live browser session against a real
/// `ladder_polls_server` proves the runtime half — `EM_JS`'s JS body is not
/// type-checked by anything at C++ compile time, and the whole point of this
/// file is a control-flow shape (`OpenPoll`'s async attach) this repository
/// has only exercised natively before now.

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QString>
#include <QUrl>
#include <QVariant>

#include "gui/app_context.hpp"
#include "poll_qml_bridges.hpp"

#include <emscripten/emscripten.h>

#include <cstdlib>
#include <memory>

namespace {

// Returns a `_malloc`'d, NUL-terminated UTF-8 copy of the `poll` query
// parameter's value, or `0` (null) if the page url has none. Freed by the
// caller with `std::free` — the same underlying allocator Emscripten's
// `_malloc` uses, per the standard EM_JS "return a JS string to C++" idiom
// (see this file's own header comment for why EM_JS rather than Embind's
// `emscripten::val`). `UTF8ToString`/`stringToUTF8`/`lengthBytesUTF8`/
// `_malloc` are Emscripten runtime internals, reachable from EM_JS-inlined
// code without needing `-sEXPORTED_RUNTIME_METHODS` (that flag only gates
// calls *into* `Module.*` from external JS, not EM_JS's own body).
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables) -- EM_JS's macro-generated shape
EM_JS(char*, pollsWasmQueryPollId, (), {
    var params = new URLSearchParams(window.location.search);
    var value = params.get('poll');
    if (value === null) {
        return 0;
    }
    var length = lengthBytesUTF8(value) + 1;
    var ptr = _malloc(length);
    stringToUTF8(value, ptr, length);
    return ptr;
});

/// @brief The `?poll=<pollId>` query parameter from the browser's current
///        url, or an empty string if the page was opened without one.
/// @return The poll id a shared link named, or `QString{}`.
[[nodiscard]] QString initialPollIdFromUrl() {
    char* raw = pollsWasmQueryPollId();
    if (raw == nullptr) {
        return QString{};
    }
    QString pollId = QString::fromUtf8(raw);
    std::free(raw);
    return pollId;
}

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication qtApp{argc, argv};

    // Always Remote — see this file's header comment. `AppContext` builds the
    // QtWebSocketBackend with asyncRegistrationEnabled=true, which is what
    // makes registration WASM-safe at all (the synchronous path nests a
    // QEventLoop and aborts the page — examples/TESTING.md, "WASM reality").
    ::morph::ladder::gui::AppContext ctx{
        ::morph::ladder::gui::Remote{.url = QUrl{QString::fromUtf8(MORPH_LADDER_POLLS_WASM_SERVER_URL)}}};

    // Read once, before the engine exists: this is a pure page-url read, not
    // a network call, so it has no readiness dependency on `ctx`.
    const QString initialPollId = initialPollIdFromUrl();

    QQmlApplicationEngine engine;
    std::unique_ptr<polls::gui::PollBridge> pollBridge;

    // Built from inside onReady(), never before it: a Remote context is not
    // usable the line after its constructor returns, and a registration or
    // attach issued before the socket is up fails permanently with no retry
    // (docs/findings/017). Identical to bookmarks'/pastebin's own Remote
    // clients, and — per this file's header comment — load-bearing here for
    // a second, distinct reason: `PollBridge`'s handler's *first* network
    // call is `OpenPoll`'s async attach itself, with no prior "registration"
    // step to race, so this is also the point past which that attach is
    // always safe to issue.
    ctx.onReady([&] {
        pollBridge = std::make_unique<polls::gui::PollBridge>(ctx.bridge(), ctx.executor());
        engine.setInitialProperties({
            {QStringLiteral("pollBridge"), QVariant::fromValue(pollBridge.get())},
            // Hides Main.qml's one route to CreatePollView (native-only) —
            // see this file's own header comment for why this is genuinely
            // unreachable, not merely dimmed.
            {QStringLiteral("nativeClient"), false},
            // Empty when the page url named no poll — Main.qml then behaves
            // exactly as before this task, starting on the landing screen.
            {QStringLiteral("initialPollId"), initialPollId},
        });
        engine.loadFromModule(MORPH_LADDER_QML_URI, "Main");
        if (engine.rootObjects().isEmpty()) {
            qWarning("ladder_polls_gui_wasm: QML engine produced no root object");
        }
    });

    qInfo("ladder_polls_gui_wasm: connecting to %s ...", MORPH_LADDER_POLLS_WASM_SERVER_URL);
    return QGuiApplication::exec();
}

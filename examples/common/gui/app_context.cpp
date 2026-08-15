// SPDX-License-Identifier: Apache-2.0
#include "gui/app_context.hpp"

#include <morph/session/session.hpp>

#include <optional>
#include <utility>

namespace morph::ladder::gui {

AppContext::AppContext(Mode mode) {
    // Built first in both modes: callbacks are delivered on the Qt thread
    // regardless of where the model work itself runs.
    _qtExecutor = std::make_unique<::morph::qt::QtExecutor>();

    if (auto* local = std::get_if<Local>(&mode)) {
        _workerPool = std::make_unique<::morph::exec::ThreadPoolExecutor>(local->workers);
        auto backend = std::make_unique<::morph::backend::LocalBackend>(*_workerPool);
        _bridge = std::make_unique<::morph::bridge::Bridge>(std::move(backend));
        // No transport to wait for: handlers may be built immediately.
        markReady();
        return;
    }

    auto& remote = std::get<Remote>(mode);
    // asyncRegistrationEnabled: the synchronous registerModel path nests a
    // QEventLoop, which aborts a WASM page outright (examples/TESTING.md,
    // "WASM reality"). Registering before the socket connects now queues and
    // retries once it does (finding 017), but this class still defers via
    // setConnectHandler below rather than registering immediately — simpler
    // to reason about than relying on the queue, and what makes the
    // readiness contract in this class's doc comment necessary.
    auto backend = std::make_unique<::morph::qt::QtWebSocketBackend>(
        remote.url,
#ifndef QT_NO_SSL
        std::nullopt,
#endif
        ::morph::qt::QtWebSocketBackend::Config{.asyncRegistrationEnabled = true});
    auto* rawBackend = backend.get();  // stays valid: the Bridge below co-owns the same object
    _bridge = std::make_unique<::morph::bridge::Bridge>(std::move(backend));

    // setConnectHandler, never waitForConnected(): the latter nests an event
    // loop and hangs a WASM page. Installed after the Bridge is built because
    // Bridge only ever installs a *reconnect* handler (bridge.hpp), so this
    // slot is ours; the handler fires on every successful connect, first one
    // included (src/qt/qt_websocket_backend.cpp's `connected` slot).
    //
    // This captures `this` without a matching teardown, which is the same
    // hazard `~Bridge()` (bridge.hpp) documents and clears for its own
    // *reconnect* handler: a co-owned backend that outlives `this` — e.g. via
    // a `shared_ptr` some other code captured from `loadBackend()` before
    // `~AppContext()` ran — could fire this handler after destruction and
    // dereference freed memory. Nothing in the ladder as shipped extends the
    // backend's lifetime that way, so this is safe in practice today, not by
    // construction; a future caller that does must not rely on this class to
    // protect them.
    rawBackend->setConnectHandler([this] { markReady(); });
}

void AppContext::onReady(std::function<void()> callback) {
    if (!callback) {
        return;
    }
    if (_ready) {
        callback();
        return;
    }
    _pendingReadyCallbacks.push_back(std::move(callback));
}

void AppContext::login(const std::string& principal) {
    _bridge->setDefaultSession(::morph::session::Context{.principal = principal});
}

void AppContext::markReady() {
    _ready = true;
    // Moved out before invoking: a callback is free to register another one
    // (which, with `_ready` already true, now runs inline rather than landing
    // in the vector this loop is iterating).
    auto callbacks = std::move(_pendingReadyCallbacks);
    _pendingReadyCallbacks.clear();
    for (auto& callback : callbacks) {
        callback();
    }
}

}  // namespace morph::ladder::gui

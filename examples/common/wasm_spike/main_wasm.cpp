// SPDX-License-Identifier: Apache-2.0
//
// WASM-remote spike: proves a WASM-compiled QtWebSocketBackend client can
// register a model and execute one action against a real remote server,
// using the two WASM-mandatory patterns documented in examples/TESTING.md,
// "WASM reality": asyncRegistrationEnabled=true (the plain synchronous
// registerModel aborts the page) and setConnectHandler (waitForConnected()
// hangs the page on WASM).
//
// This binary is the client half only — point MORPH_LADDER_WASM_SPIKE_SERVER_URL
// (baked in at build time via a CMake compile definition, since a browser
// page cannot read environment variables) at a real morph::qt::RemoteServer +
// QtWebSocketServer hosting SpikeEchoModel, started out-of-band (see this
// directory's README.md for how the nightly Playwright smoke wires that up).
//
// IMPORTANT ordering constraint discovered while building this spike:
// QtWebSocketBackend::registerModelAsync() now queues a registration issued
// before the socket has connected and retries it once the connection comes
// up (docs/spec/core/backend.md, "Asynchronous registration") -- but the
// *reconnect* handler Bridge installs only fires on a *subsequent*
// reconnect, never on the first connect, so this spike still defers to
// setConnectHandler rather than relying on the pre-connect queue. The
// registering call (here, constructing the BridgeHandler, whose constructor
// itself registers) is deferred to fire from inside the `setConnectHandler`
// callback instead, which is fully WASM-safe (no nested event loop) and
// simpler to reason about than the queue.

#include <QCoreApplication>
#include <QDebug>
#include <QTimer>
#include <memory>
#include <morph/core/bridge.hpp>
#include <morph/qt/qt_executor.hpp>
#include <morph/qt/qt_websocket_backend.hpp>
#include <optional>
#include <utility>

#include "spike_model.hpp"

BRIDGE_REGISTER_MODEL(SpikeEchoModel, "SpikeEchoModel")
BRIDGE_REGISTER_ACTION(SpikeEchoModel, SpikeEchoAction, "SpikeEchoAction")

int main(int argc, char* argv[]) {
    QCoreApplication app{argc, argv};

    QUrl url{QStringLiteral(MORPH_LADDER_WASM_SPIKE_SERVER_URL)};
    // QtWebSocketBackend's constructor has no `tls` parameter at all on an
    // SSL-less Qt build (QT_NO_SSL) -- see its class doc comment's "SSL-less
    // Qt builds" section. A WASM build is always QT_NO_SSL, so the 4th
    // positional argument here is `cfg`, not `tls`; passing `std::nullopt`
    // unconditionally (as if `tls` always existed) is a link-time-only bug
    // that never surfaces on a native build, where `QT_NO_SSL` is unset --
    // mirrors backend_rig.hpp's identical split for QtWebSocketServer.
#ifdef QT_NO_SSL
    auto backendPtr = std::make_unique<morph::qt::QtWebSocketBackend>(
        url, morph::qt::QtWebSocketBackend::Config{.asyncRegistrationEnabled = true});
#else
    auto backendPtr = std::make_unique<morph::qt::QtWebSocketBackend>(
        url, std::nullopt, morph::qt::QtWebSocketBackend::Config{.asyncRegistrationEnabled = true});
#endif
    auto* rawBackend = backendPtr.get();  // stays valid: Bridge below co-owns the same object

    morph::qt::QtExecutor qtExec;
    morph::bridge::Bridge bridge{std::move(backendPtr)};

    // Holds the one BridgeHandler this spike ever constructs. Must outlive
    // the timer lambda below: a lambda-local BridgeHandler is destroyed the
    // instant its enclosing lambda invocation returns, and ~BridgeHandler()
    // deregisters the model (resetting its binding's currentId to 0) --
    // which would race the still-in-flight server reply to the execute()
    // call the same lambda just made.
    std::optional<morph::bridge::BridgeHandler<SpikeEchoModel>> handler;

    // waitForConnected() would nest an event loop and abort the page on WASM
    // (TESTING.md, "WASM reality") — setConnectHandler is the mandated
    // substitute. Constructing BridgeHandler (whose default constructor
    // registers against the default model factory) here, not before, is
    // what the ordering-constraint comment above requires: this is the
    // earliest point at which the async registration call is guaranteed to
    // see a live connection.
    rawBackend->setConnectHandler([&bridge, &qtExec, &handler] {
        qDebug() << "morph-ladder-wasm-spike: connected";
        handler.emplace(bridge, &qtExec);
    });

    // Poll (via a QTimer, not waitForConnected/pumpUntil — this is real page
    // code, not a test) until the async registration completes, then fire
    // one action and log the result to the browser console, where the
    // nightly Playwright smoke (this directory's README) asserts on it.
    // `BridgeHandler::isBound()` observes the same registration settlement
    // that used to require polling `HandlerBinding::currentId` directly, so
    // this spike never names `morph::bridge::detail::HandlerBinding` — an
    // internal record that is deliberately not public API
    // (`docs/spec/core/bridge.md`, "`HandlerBinding`").
    auto* timer = new QTimer{&app};
    QObject::connect(timer, &QTimer::timeout, [&handler] {
        if (!handler.has_value() || !handler->isBound()) {
            return;
        }
        static bool fired = false;
        if (fired) {
            return;
        }
        fired = true;
        handler->execute(SpikeEchoAction{99})
            .then([](int value) { qDebug() << "morph-ladder-wasm-spike: result=" << value; })
            .onError([](const std::exception_ptr&) { qDebug() << "morph-ladder-wasm-spike: error"; });
    });
    timer->start(50);

    return app.exec();
}

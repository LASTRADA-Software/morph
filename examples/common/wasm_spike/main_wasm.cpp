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
// IMPORTANT ordering constraint discovered while building this spike (see
// docs/findings/017-async-registration-fails-before-connect.md):
// QtWebSocketBackend::registerModelAsync() fails immediately (onError
// "disconnected") with no retry/queueing if called before the socket has
// actually connected — and the *reconnect* handler Bridge installs only
// fires on a *subsequent* reconnect, never on the first connect. So the
// registering call (here, constructing the BridgeHandler, whose constructor
// itself registers) must not happen unconditionally right after constructing
// the Bridge (that would happen synchronously, before any event-loop turn,
// so the socket is guaranteed not yet connected) — it is deferred here to
// fire from inside the `setConnectHandler` callback instead, which is itself
// still fully WASM-safe (no nested event loop).

#include "spike_model.hpp"

#include <QCoreApplication>
#include <QDebug>
#include <QTimer>
#include <morph/core/bridge.hpp>
#include <morph/core/registry.hpp>
#include <morph/qt/qt_executor.hpp>
#include <morph/qt/qt_websocket_backend.hpp>

#include <memory>
#include <optional>
#include <utility>

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
        url, morph::model::detail::defaultDispatcher(), morph::model::detail::defaultRegistry(),
        morph::qt::QtWebSocketBackend::Config{.asyncRegistrationEnabled = true});
#else
    auto backendPtr = std::make_unique<morph::qt::QtWebSocketBackend>(
        url, morph::model::detail::defaultDispatcher(), morph::model::detail::defaultRegistry(), std::nullopt,
        morph::qt::QtWebSocketBackend::Config{.asyncRegistrationEnabled = true});
#endif
    auto* rawBackend = backendPtr.get();  // stays valid: Bridge below co-owns the same object

    auto binding = std::make_shared<morph::bridge::detail::HandlerBinding>();
    binding->typeId = "SpikeEchoModel";
    binding->modelFactory = [] { return morph::model::detail::ModelFactory::create<SpikeEchoModel>(); };

    morph::qt::QtExecutor qtExec;
    morph::bridge::Bridge bridge{std::move(backendPtr)};

    // Holds the one BridgeHandler this spike ever constructs. Must outlive
    // the timer lambda below: a lambda-local BridgeHandler is destroyed the
    // instant its enclosing lambda invocation returns, and ~BridgeHandler()
    // deregisters the model (resetting binding->currentId to 0) -- which
    // would race the still-in-flight server reply to the execute() call the
    // same lambda just made.
    std::optional<morph::bridge::BridgeHandler<SpikeEchoModel>> handler;

    // waitForConnected() would nest an event loop and abort the page on WASM
    // (TESTING.md, "WASM reality") — setConnectHandler is the mandated
    // substitute. Constructing BridgeHandler (whose constructor itself calls
    // Bridge::registerHandler(binding) -- see bridge.hpp's
    // BridgeHandler(Bridge&, IExecutor*, shared_ptr<HandlerBinding>)
    // overload) here, not before, is what the ordering-constraint comment
    // above requires: this is the earliest point at which the async
    // registration call is guaranteed to see a live connection. This also
    // replaces what would otherwise be a duplicate registration (once here,
    // once implicitly via a separate Bridge::registerHandler(binding) call).
    rawBackend->setConnectHandler([&bridge, &qtExec, &handler, binding] {
        qDebug() << "morph-ladder-wasm-spike: connected";
        handler.emplace(bridge, &qtExec, binding);
    });

    // Poll (via a QTimer, not waitForConnected/pumpUntil — this is real page
    // code, not a test) until the async registration completes, then fire
    // one action and log the result to the browser console, where the
    // nightly Playwright smoke (this directory's README) asserts on it.
    auto* timer = new QTimer{&app};
    QObject::connect(timer, &QTimer::timeout, [&binding, &handler] {
        if (binding->currentId.load() == 0U) {
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

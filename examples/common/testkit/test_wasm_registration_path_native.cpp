// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include "testkit/pump.hpp"

#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <morph/qt/qt_executor.hpp>
#include <morph/qt/qt_websocket_backend.hpp>
#include <morph/qt/qt_websocket_server.hpp>

#include <chrono>
#include <memory>
#include <utility>

// Deliberately at namespace scope, not inside an anonymous namespace: glz's
// reflection (which BRIDGE_REGISTER_MODEL/BRIDGE_REGISTER_ACTION rely on to
// serialize these types across the wire) needs external linkage on the
// type — see glaze/reflection/get_name.hpp's `extern const T external`, and
// test_fault_proxy.cpp's/test_backend_rig.cpp's identical note. Distinctly
// named from wasm_spike/spike_model.hpp's SpikeEchoModel/SpikeEchoAction:
// this test target and main_wasm.cpp's registration would violate ODR if
// ever linked into the same process (wasm_spike/spike_model.hpp's own
// comment), so this test uses its own model instead of reusing that one.
struct WasmSpikeProbeAction {
    int value = 0;
};
struct WasmSpikeProbeModel {
    int execute(WasmSpikeProbeAction action) { return action.value; }
};

BRIDGE_REGISTER_MODEL(WasmSpikeProbeModel, "WasmSpikeProbeModel")
BRIDGE_REGISTER_ACTION(WasmSpikeProbeModel, WasmSpikeProbeAction, "WasmSpikeProbeAction")

// The brief's original draft for this test (and main_wasm.cpp's first draft)
// called `bridge.registerHandler(binding)` unconditionally, immediately after
// constructing the Bridge -- before any Qt event-loop turn had a chance to
// run, so the QWebSocket was guaranteed to still be unconnected at that
// point. `QtWebSocketBackend::registerModelAsync()` now queues a
// pre-connect registration and retries it once the socket connects (see
// tests/qt/test_qt_websocket.cpp's "registerModelAsync called before the
// socket connects queues and retries once connected fires"), closing the
// gap docs/findings/017-async-registration-fails-before-connect.md
// originally documented -- so this call sequence now resolves natively,
// with no need for the deferred-registerHandler workaround the test below
// demonstrates (which remains a valid, simpler-still sequence, just no
// longer the only correct one).
TEST_CASE("registerHandler() called immediately after Bridge construction, before any event-loop turn, resolves "
          "once the socket connects -- see finding 017",
          "[ladder][testkit][wasm-spike]") {
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    morph::qt::QtWebSocketServer wsServer{*server, 0};
    REQUIRE(wsServer.listen());

    QUrl url{QString("ws://127.0.0.1:%1").arg(wsServer.port())};
    auto backendPtr = std::make_unique<morph::qt::QtWebSocketBackend>(
        url, morph::model::detail::defaultDispatcher(), morph::model::detail::defaultRegistry(), std::nullopt,
        morph::qt::QtWebSocketBackend::Config{.asyncRegistrationEnabled = true});

    morph::qt::QtExecutor qtExec;
    morph::bridge::Bridge bridge{std::move(backendPtr)};

    auto binding = std::make_shared<morph::bridge::detail::HandlerBinding>();
    binding->typeId = "WasmSpikeProbeModel";
    binding->modelFactory = [] { return morph::model::detail::ModelFactory::create<WasmSpikeProbeModel>(); };
    bridge.registerHandler(binding);  // called before the socket is connected -- see finding 017

    REQUIRE(morph::ladder::testkit::pumpUntil([&] { return binding->currentId.load() != 0U; }));
}

// The corrected, still fully WASM-safe sequence: defer `registerHandler()`
// until `setConnectHandler`'s callback has actually fired at least once --
// no `waitForConnected()` (which would nest an event loop and abort a WASM
// page), just ordering the same non-blocking calls correctly. main_wasm.cpp
// uses this exact corrected sequence (see its file comment for the same
// explanation).
TEST_CASE("The WASM spike's registration call sequence resolves natively when registerHandler() is deferred to "
          "setConnectHandler's callback (asyncRegistrationEnabled + setConnectHandler)",
          "[ladder][testkit][wasm-spike]") {
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    morph::qt::QtWebSocketServer wsServer{*server, 0};
    REQUIRE(wsServer.listen());

    QUrl url{QString("ws://127.0.0.1:%1").arg(wsServer.port())};
    auto backendPtr = std::make_unique<morph::qt::QtWebSocketBackend>(
        url, morph::model::detail::defaultDispatcher(), morph::model::detail::defaultRegistry(), std::nullopt,
        morph::qt::QtWebSocketBackend::Config{.asyncRegistrationEnabled = true});
    auto* rawBackend = backendPtr.get();  // stays valid: bridge below co-owns the same object

    auto binding = std::make_shared<morph::bridge::detail::HandlerBinding>();
    binding->typeId = "WasmSpikeProbeModel";
    binding->modelFactory = [] { return morph::model::detail::ModelFactory::create<WasmSpikeProbeModel>(); };

    morph::qt::QtExecutor qtExec;
    morph::bridge::Bridge bridge{std::move(backendPtr)};

    // Installed after Bridge takes ownership (via the raw pointer captured
    // above) but before any event-loop turn runs, so it cannot miss the
    // connect signal -- identical pattern to main_wasm.cpp.
    rawBackend->setConnectHandler([&bridge, binding] { bridge.registerHandler(binding); });

    REQUIRE(morph::ladder::testkit::pumpUntil([&] { return binding->currentId.load() != 0U; }));

    morph::bridge::BridgeHandler<WasmSpikeProbeModel> handler{bridge, &qtExec, binding};
    auto result = morph::ladder::testkit::awaitQt(handler.execute(WasmSpikeProbeAction{99}));
    REQUIRE(result == 99);
}

// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <memory>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/remote.hpp>
#include <morph/qt/qt_executor.hpp>
#include <morph/qt/qt_websocket_backend.hpp>
#include <morph/qt/qt_websocket_server.hpp>
#include <optional>
#include <utility>

#include "testkit/pump.hpp"

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

// A `BridgeHandler` constructed immediately after the Bridge, before any Qt
// event-loop turn, meets a socket that is guaranteed to be unconnected.
// `QtWebSocketBackend::bindModel()` queues that private bind and sends it
// once the socket connects, so the handler becomes bound without the caller
// deferring its construction. `isBound()` observes the settlement.
TEST_CASE(
    "registerHandler() called immediately after Bridge construction, before any event-loop turn, resolves "
    "once the socket connects",
    "[ladder][testkit][wasm-spike]") {
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    morph::qt::QtWebSocketServer wsServer{*server, 0};
    REQUIRE(wsServer.listen());

    QUrl url{QString("ws://127.0.0.1:%1").arg(wsServer.port())};
    auto backendPtr = std::make_unique<morph::qt::QtWebSocketBackend>(
        url, std::nullopt, morph::qt::QtWebSocketBackend::Config{.asyncRegistrationEnabled = true});

    morph::qt::QtExecutor qtExec;
    morph::bridge::Bridge bridge{std::move(backendPtr), qtExec};

    // Constructing the handler registers immediately -- before the socket is
    // connected -- via the queue-and-retry path described above.
    morph::bridge::BridgeHandler<WasmSpikeProbeModel> handler{bridge, &qtExec};

    REQUIRE(morph::ladder::testkit::pumpUntil([&] { return handler.isBound(); }));
}

// The corrected, still fully WASM-safe sequence: defer constructing the
// `BridgeHandler` (whose constructor registers) until `setConnectHandler`'s
// callback has actually fired at least once -- no `waitForConnected()`
// (which would nest an event loop and abort a WASM page), just ordering the
// same non-blocking calls correctly. main_wasm.cpp uses this exact corrected
// sequence (see its file comment for the same explanation), including the
// `std::optional<BridgeHandler<Model>>` deferred-construction idiom, since a
// handler cannot be built before there is somewhere to register it into yet
// must still exist afterward to `execute()` against.
TEST_CASE(
    "The WASM spike's registration call sequence resolves natively when registerHandler() is deferred to "
    "setConnectHandler's callback (asyncRegistrationEnabled + setConnectHandler)",
    "[ladder][testkit][wasm-spike]") {
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    morph::qt::QtWebSocketServer wsServer{*server, 0};
    REQUIRE(wsServer.listen());

    QUrl url{QString("ws://127.0.0.1:%1").arg(wsServer.port())};
    auto backendPtr = std::make_unique<morph::qt::QtWebSocketBackend>(
        url, std::nullopt, morph::qt::QtWebSocketBackend::Config{.asyncRegistrationEnabled = true});
    auto* rawBackend = backendPtr.get();  // stays valid: bridge below co-owns the same object

    morph::qt::QtExecutor qtExec;
    morph::bridge::Bridge bridge{std::move(backendPtr), qtExec};

    std::optional<morph::bridge::BridgeHandler<WasmSpikeProbeModel>> handler;
    // Installed after Bridge takes ownership (via the raw pointer captured
    // above) but before any event-loop turn runs, so it cannot miss the
    // connect signal -- identical pattern to main_wasm.cpp.
    rawBackend->setConnectHandler([&bridge, &qtExec, &handler] { handler.emplace(bridge, &qtExec); });

    REQUIRE(morph::ladder::testkit::pumpUntil([&] { return handler.has_value() && handler->isBound(); }));

    auto result = morph::ladder::testkit::awaitQt(handler->execute(WasmSpikeProbeAction{99}));
    REQUIRE(result == 99);
}

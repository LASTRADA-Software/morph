// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <functional>
#include <memory>
#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <morph/net/socket_backend.hpp>
#include <morph/net/socket_server.hpp>
#include <stdexcept>
#include <string>
#include <thread>

// Deliberately NOT in an anonymous namespace: glaze's reflection-based
// get_name() needs these types to have external linkage (see
// tests/qt/test_qt_websocket.cpp's WsEchoAction/WsEchoModel for the same
// convention).
struct SbEchoAction {
    int value = 0;
};
struct SbEchoFail {};

struct SbEchoModel {
    int execute(SbEchoAction action) { return action.value; }
    int execute(SbEchoFail) { throw std::runtime_error("echo failed"); }
};

BRIDGE_REGISTER_MODEL(SbEchoModel, "SbEchoModel")
BRIDGE_REGISTER_ACTION(SbEchoModel, SbEchoAction, "SbEchoAction")
BRIDGE_REGISTER_ACTION(SbEchoModel, SbEchoFail, "SbEchoFail")

struct SbSlowAction {
    int value = 0;
};

struct SbSlowModel {
    int execute(SbSlowAction action) {
        std::this_thread::sleep_for(std::chrono::milliseconds{300});
        return action.value;
    }
};

BRIDGE_REGISTER_MODEL(SbSlowModel, "SbSlowModel")
BRIDGE_REGISTER_ACTION(SbSlowModel, SbSlowAction, "SbSlowAction")

namespace {
void spinUntil(const std::function<bool()>& done, int maxIterations = 200) {
    for (int i = 0; i < maxIterations && !done(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
}
}  // namespace

TEST_CASE("SocketBackend: action result delivered via then", "[net][socket_backend]") {
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    morph::net::SocketServer wsServer{*server, 0};
    REQUIRE(wsServer.listen());

    std::string const url = "ws://127.0.0.1:" + std::to_string(static_cast<unsigned>(wsServer.port()));
    auto backendPtr = std::make_unique<morph::net::SocketBackend>(url);
    REQUIRE(backendPtr->waitForConnected());

    morph::exec::ThreadPoolExecutor cbPool{1};
    morph::bridge::Bridge bridge{std::move(backendPtr)};
    morph::bridge::BridgeHandler<SbEchoModel> handler{bridge, &cbPool};

    std::atomic<int> result{-1};
    handler.execute(SbEchoAction{99}).then([&](int val) { result.store(val); }).onError([](const std::exception_ptr&) {
    });

    spinUntil([&] { return result.load() != -1; });
    REQUIRE(result.load() == 99);
}

TEST_CASE("SocketBackend: exception delivered via onError", "[net][socket_backend]") {
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    morph::net::SocketServer wsServer{*server, 0};
    REQUIRE(wsServer.listen());

    std::string const url = "ws://127.0.0.1:" + std::to_string(static_cast<unsigned>(wsServer.port()));
    auto backendPtr = std::make_unique<morph::net::SocketBackend>(url);
    REQUIRE(backendPtr->waitForConnected());

    morph::exec::ThreadPoolExecutor cbPool{1};
    morph::bridge::Bridge bridge{std::move(backendPtr)};
    morph::bridge::BridgeHandler<SbEchoModel> handler{bridge, &cbPool};

    std::atomic<bool> errorFired{false};
    handler.execute(SbEchoFail{}).then([](int) {}).onError([&](const std::exception_ptr& exc) {
        try {
            std::rethrow_exception(exc);
        } catch (const std::runtime_error&) {
            errorFired.store(true);
        }
    });

    spinUntil([&] { return errorFired.load(); });
    REQUIRE(errorFired.load());
}

TEST_CASE("SocketBackend: many concurrent in-flight executes all resolve, matched by callId",
          "[net][socket_backend]") {
    morph::exec::ThreadPoolExecutor serverPool{4};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    morph::net::SocketServer wsServer{*server, 0};
    REQUIRE(wsServer.listen());

    std::string const url = "ws://127.0.0.1:" + std::to_string(static_cast<unsigned>(wsServer.port()));
    auto backendPtr = std::make_unique<morph::net::SocketBackend>(url);
    REQUIRE(backendPtr->waitForConnected());

    morph::exec::ThreadPoolExecutor cbPool{2};
    morph::bridge::Bridge bridge{std::move(backendPtr)};
    morph::bridge::BridgeHandler<SbEchoModel> handler{bridge, &cbPool};

    constexpr int numCalls = 40;
    std::atomic<int> resolved{0};
    std::atomic<long long> sum{0};
    for (int i = 1; i <= numCalls; ++i) {
        handler.execute(SbEchoAction{i})
            .then([&](int val) {
                sum.fetch_add(val);
                resolved.fetch_add(1);
            })
            .onError([](const std::exception_ptr&) {});
    }
    spinUntil([&] { return resolved.load() == numCalls; }, 500);
    REQUIRE(resolved.load() == numCalls);
    REQUIRE(sum.load() == (numCalls * (numCalls + 1)) / 2);
}

TEST_CASE("SocketBackend: two backends share one server with isolated model state", "[net][socket_backend]") {
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    morph::net::SocketServer wsServer{*server, 0};
    REQUIRE(wsServer.listen());

    std::string const url = "ws://127.0.0.1:" + std::to_string(static_cast<unsigned>(wsServer.port()));
    auto backendA = std::make_unique<morph::net::SocketBackend>(url);
    auto backendB = std::make_unique<morph::net::SocketBackend>(url);
    REQUIRE(backendA->waitForConnected());
    REQUIRE(backendB->waitForConnected());

    morph::exec::ThreadPoolExecutor cbPool{2};
    morph::bridge::Bridge bridgeA{std::move(backendA)};
    morph::bridge::Bridge bridgeB{std::move(backendB)};
    morph::bridge::BridgeHandler<SbEchoModel> handlerA{bridgeA, &cbPool};
    morph::bridge::BridgeHandler<SbEchoModel> handlerB{bridgeB, &cbPool};

    std::atomic<int> lastA{-1};
    std::atomic<int> lastB{-1};
    handlerA.execute(SbEchoAction{11}).then([&](int v) { lastA.store(v); }).onError([](const std::exception_ptr&) {});
    handlerB.execute(SbEchoAction{22}).then([&](int v) { lastB.store(v); }).onError([](const std::exception_ptr&) {});

    spinUntil([&] { return lastA.load() != -1 && lastB.load() != -1; });
    REQUIRE(lastA.load() == 11);
    REQUIRE(lastB.load() == 22);
}

TEST_CASE("SocketBackend: registerModel on a never-connected socket throws, does not hang",
          "[net][socket_backend][disconnect]") {
    // Port 1 is reserved (root-only) on Linux/macOS and never listening — the
    // socket never connects, so the io thread exits without retrying.
    morph::net::SocketBackend backend{"ws://127.0.0.1:1"};
    REQUIRE_FALSE(backend.waitForConnected(std::chrono::milliseconds{200}));

    bool threw = false;
    std::string what;
    try {
        (void)backend.registerModel("SbEchoModel", nullptr);
    } catch (const std::exception& exc) {
        threw = true;
        what = exc.what();
    }
    REQUIRE(threw);
    REQUIRE(what.find("register failed") != std::string::npos);
    REQUIRE(what.find("disconnected") != std::string::npos);
}

TEST_CASE("SocketBackend: register after the server closes fails instead of hanging",
          "[net][socket_backend][disconnect]") {
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    auto wsServer = std::make_unique<morph::net::SocketServer>(*server, 0);
    REQUIRE(wsServer->listen());

    std::string const url = "ws://127.0.0.1:" + std::to_string(static_cast<unsigned>(wsServer->port()));
    morph::net::SocketBackend backend{url};
    REQUIRE(backend.waitForConnected());

    auto mid = backend.registerModel("SbEchoModel", nullptr);
    REQUIRE(mid.v != 0U);

    wsServer->close();
    wsServer.reset();

    bool threw = false;
    std::string what;
    for (int i = 0; i < 100 && !threw; ++i) {
        try {
            (void)backend.registerModel("SbEchoModel", nullptr);
        } catch (const std::exception& exc) {
            threw = true;
            what = exc.what();
        }
        if (!threw) {
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
        }
    }
    REQUIRE(threw);
    REQUIRE(what.find("register failed") != std::string::npos);
}

TEST_CASE("SocketBackend: execute while disconnected resolves immediately with DisconnectedError",
          "[net][socket_backend][disconnect]") {
    morph::net::SocketBackend backend{"ws://127.0.0.1:1"};
    REQUIRE_FALSE(backend.waitForConnected(std::chrono::milliseconds{200}));

    morph::exec::ThreadPoolExecutor cbPool{1};
    morph::backend::detail::ActionCall call;
    call.modelTypeId = "SbEchoModel";
    call.actionTypeId = "SbEchoAction";
    call.serializeAction = [] { return std::string{"{}"}; };
    call.deserializeResult = [](std::string_view) -> std::shared_ptr<void> { return nullptr; };

    std::atomic<bool> gotDisconnected{false};
    auto comp = backend.execute(morph::exec::detail::ModelId{1}, std::move(call), &cbPool);
    comp.onError([&](const std::exception_ptr& exc) {
        try {
            std::rethrow_exception(exc);
        } catch (const morph::backend::DisconnectedError&) {
            gotDisconnected.store(true);
        }
    });
    spinUntil([&] { return gotDisconnected.load(); });
    REQUIRE(gotDisconnected.load());
}

TEST_CASE("SocketBackend: server dropping mid-call resolves the pending completion with DisconnectedError",
          "[net][socket_backend][disconnect]") {
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    auto wsServer = std::make_unique<morph::net::SocketServer>(*server, 0);
    REQUIRE(wsServer->listen());

    std::string const url = "ws://127.0.0.1:" + std::to_string(static_cast<unsigned>(wsServer->port()));
    auto backendPtr = std::make_unique<morph::net::SocketBackend>(url);
    REQUIRE(backendPtr->waitForConnected());

    morph::exec::ThreadPoolExecutor cbPool{1};
    morph::bridge::Bridge bridge{std::move(backendPtr)};
    morph::bridge::BridgeHandler<SbSlowModel> handler{bridge, &cbPool};

    std::atomic<bool> gotDisconnected{false};
    handler.execute(SbSlowAction{5}).then([](int) {}).onError([&](const std::exception_ptr& exc) {
        try {
            std::rethrow_exception(exc);
        } catch (const morph::backend::DisconnectedError&) {
            gotDisconnected.store(true);
        }
    });

    // Give the request time to reach the server and start the slow action,
    // then pull the rug out from under the connection before it replies.
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    wsServer->close();
    wsServer.reset();

    spinUntil([&] { return gotDisconnected.load(); }, 200);
    REQUIRE(gotDisconnected.load());
}

TEST_CASE("SocketBackend: reconnects to a fresh server on the same port", "[net][socket_backend][disconnect]") {
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);

    morph::net::SocketBackend::Config cfg;
    cfg.initialReconnectDelay = std::chrono::milliseconds{50};
    cfg.maxReconnectDelay = std::chrono::milliseconds{200};

    std::uint16_t port = 0;
    std::string url;
    std::unique_ptr<morph::net::SocketBackend> backend;
    {
        auto wsServer = std::make_unique<morph::net::SocketServer>(*server, 0);
        REQUIRE(wsServer->listen());
        port = wsServer->port();
        url = "ws://127.0.0.1:" + std::to_string(static_cast<unsigned>(port));

        backend = std::make_unique<morph::net::SocketBackend>(url, cfg);
        REQUIRE(backend->waitForConnected());

        auto mid = backend->registerModel("SbEchoModel", nullptr);
        REQUIRE(mid.v != 0U);
        // wsServer is destroyed at the end of this scope.
    }
    // Give the OS a moment to release the port before rebinding it.
    std::this_thread::sleep_for(std::chrono::milliseconds{100});

    auto wsServer2 = std::make_unique<morph::net::SocketServer>(*server, port);
    REQUIRE(wsServer2->listen());

    // _connected is a level-triggered flag, so polling waitForConnected
    // repeatedly is correct: it returns true as soon as the io thread's
    // backoff loop reconnects (50ms initial delay, 200ms cap).
    bool reconnected = false;
    for (int i = 0; i < 100 && !reconnected; ++i) {
        if (backend->waitForConnected(std::chrono::milliseconds{50})) {
            reconnected = true;
        }
    }
    REQUIRE(reconnected);

    auto mid2 = backend->registerModel("SbEchoModel", nullptr);
    REQUIRE(mid2.v != 0U);
}

namespace {

// Shared scaffolding for the reconnect tests: brings a server up, connects a
// backend, installs `onReconnect`, then drops the server and starts a fresh one
// on the same port so the backend's retry loop fires the handler. Extracted so
// each test below is just its own assertions.
struct ReconnectFixture {
    morph::exec::ThreadPoolExecutor serverPool{2};
    std::shared_ptr<morph::backend::RemoteServer> server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    std::unique_ptr<morph::net::SocketBackend> backend;
    std::unique_ptr<morph::net::SocketServer> restarted;
    std::uint16_t port = 0;

    void bounce(const std::function<void()>& onReconnect) {
        morph::net::SocketBackend::Config cfg;
        cfg.initialReconnectDelay = std::chrono::milliseconds{50};
        cfg.maxReconnectDelay = std::chrono::milliseconds{200};
        {
            auto first = std::make_unique<morph::net::SocketServer>(*server, 0);
            if (!first->listen()) {
                throw std::runtime_error("ReconnectFixture: initial listen failed");
            }
            port = first->port();
            backend = std::make_unique<morph::net::SocketBackend>(
                "ws://127.0.0.1:" + std::to_string(static_cast<unsigned>(port)), cfg);
            if (!backend->waitForConnected()) {
                throw std::runtime_error("ReconnectFixture: initial connect failed");
            }
            backend->setReconnectHandler(onReconnect);
        }  // first server destroyed -> the backend observes the drop and retries

        std::this_thread::sleep_for(std::chrono::milliseconds{100});
        restarted = std::make_unique<morph::net::SocketServer>(*server, port);
        if (!restarted->listen()) {
            throw std::runtime_error("ReconnectFixture: re-listen failed");
        }
    }
};

}  // namespace

TEST_CASE("SocketBackend: a reconnect handler that re-registers does not deadlock the transport",
          "[net][socket_backend][disconnect]") {
    // The handler used to run inline on the I/O thread from onConnected(),
    // *before* readLoop() started. Any handler doing what a reconnect handler
    // exists to do -- re-registering its models, which Bridge does via sendSync
    // -- then blocked on _syncCv waiting for a reply only readLoop could
    // deliver, on the very thread that was supposed to start readLoop. The wait
    // has no timeout, so the transport wedged permanently.
    //
    // This test therefore fails by *hanging* on the old code, which ctest's
    // per-test TIMEOUT turns into a failure.
    ReconnectFixture fixture;
    std::atomic<bool> handlerRan{false};
    std::atomic<bool> handlerSucceeded{false};

    fixture.bounce([&] {
        handlerRan.store(true);
        // The synchronous control call that used to deadlock here.
        handlerSucceeded.store(fixture.backend->registerModel("SbEchoModel", nullptr).v != 0U);
    });

    spinUntil([&] { return handlerSucceeded.load(); }, 500);
    CHECK(handlerRan.load());
    CHECK(handlerSucceeded.load());

    // The transport is still fully usable afterwards -- the handler's sendSync
    // resolved rather than leaving _syncInFlight stuck set.
    CHECK(fixture.backend->registerModel("SbEchoModel", nullptr).v != 0U);
}

TEST_CASE("SocketBackend: a throwing reconnect handler leaves the transport usable",
          "[net][socket_backend][disconnect]") {
    // The handler now runs on its own thread; an exception escaping it must not
    // terminate that thread, or the *next* reconnect would silently never fire.
    ReconnectFixture fixture;
    std::atomic<int> handlerCalls{0};

    fixture.bounce([&] {
        handlerCalls.fetch_add(1);
        throw std::runtime_error("handler blew up");
    });

    spinUntil([&] { return handlerCalls.load() > 0; }, 500);
    CHECK(handlerCalls.load() > 0);
    CHECK(fixture.backend->registerModel("SbEchoModel", nullptr).v != 0U);
}

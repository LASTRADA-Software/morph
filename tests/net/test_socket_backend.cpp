// SPDX-License-Identifier: Apache-2.0

#include <sys/socket.h>

#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <chrono>
#include <functional>
#include <memory>
#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <morph/core/wire.hpp>
#include <morph/net/detail/tcp_socket.hpp>
#include <morph/net/detail/ws_frame.hpp>
#include <morph/net/detail/ws_handshake.hpp>
#include <morph/net/socket_backend.hpp>
#include <morph/net/socket_server.hpp>
#include <morph/session/session.hpp>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

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

// A stateful, keyed model: two clients naming the same key must reach one
// instance and see one counter. A stateless echo model could not tell the
// difference between sharing and not sharing.
//
// External linkage as above: glaze reflection and the BRIDGE_REGISTER_* macros
// both require it.
// NOLINTBEGIN(misc-use-internal-linkage)
struct SbBump {
    std::int64_t id = 0;
    int by = 0;
};
struct SbTotal {
    int value = 0;
};

struct SbCounterModel {
    int value = 0;
    SbTotal execute(const SbBump& act) {
        value += act.by;
        return {.value = value};
    }
};

BRIDGE_REGISTER_MODEL(SbCounterModel, "SbCounterModel")
BRIDGE_REGISTER_ACTION(SbCounterModel, SbBump, "SbBump")
BRIDGE_MODEL_KEY(SbCounterModel, SbBump, &SbBump::id);
// NOLINTEND(misc-use-internal-linkage)

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

// Polls until `backend` reports disconnected, or `maxIterations` * spinUntil's
// own 10ms step elapses (200 -> ~2s; 25 -> ~250ms for the tighter
// abortive-close races below, which already run inside their own 40-iteration
// outer loop). waitForConnected()'s own wait_for predicate is already
// satisfied while _connected is still true, so polling it with a zero timeout
// alone would spin through every iteration in a few microseconds, never
// giving the io thread a chance to notice the disconnect -- spinUntil's real
// wall-clock sleep between checks is what actually gives it that chance.
bool waitForDisconnect(morph::net::SocketBackend& backend, int maxIterations = 200) {
    spinUntil([&] { return !backend.waitForConnected(std::chrono::milliseconds{0}); }, maxIterations);
    return !backend.waitForConnected(std::chrono::milliseconds{0});
}

// Rejects every authorize()/authorizeRegister() call. Used to force the
// server-side `err "unauthorized"` reply SocketBackend's control-call error
// paths (registerModel/sendControlForId/listInstances) otherwise never see
// from a real, unconfigured RemoteServer.
struct DenyAllAuthorizer : morph::session::IAuthorizer {
    [[nodiscard]] bool authorize(const morph::session::Context&, std::string_view, std::string_view) const override {
        return false;
    }
    [[nodiscard]] bool authorizeRegister(const morph::session::Context&, std::string_view) const override {
        return false;
    }
};

// Delays authorizeRegister() by a fixed amount, so a client's synchronous
// control call (registerModel/attach/...) is provably still parked waiting
// for the reply -- not already past it -- when a test wants to act (close
// the server, race a second call) while it's genuinely in flight.
struct SlowAuthorizer : morph::session::IAuthorizer {
    std::chrono::milliseconds delay;
    explicit SlowAuthorizer(std::chrono::milliseconds d) : delay(d) {}
    [[nodiscard]] bool authorize(const morph::session::Context&, std::string_view, std::string_view) const override {
        return true;
    }
    [[nodiscard]] bool authorizeRegister(const morph::session::Context&, std::string_view) const override {
        std::this_thread::sleep_for(delay);
        return true;
    }
};

// A fake WebSocket peer that plays the *server* role against a real
// SocketBackend, so tests can inject malformed/malicious server behavior a
// real SocketServer/RemoteServer never produces on its own (garbage
// envelopes, unmatched callIds, raw control frames, broken WS framing).
// Mirrors RawWsClient in test_socket_server.cpp, which plays the opposite
// (client) role against a real SocketServer.
class FakeWsServer {
public:
    FakeWsServer() : _listener(morph::net::detail::TcpSocket::listen(0)) {}

    [[nodiscard]] std::uint16_t port() const { return _listener.boundPort(); }

    // Accepts the pending connection and completes a real WS handshake.
    // Blocking, but the client (a SocketBackend under test) is already
    // connecting concurrently on its own io thread by the time this is
    // called, so it returns promptly.
    void acceptAndHandshake() {
        _socket = _listener.accept();
        std::string const leftover = morph::net::detail::performServerHandshake(_socket);
        _reader.feed(leftover);
    }

    void sendFrame(morph::net::detail::WsOpcode opcode, std::string_view payload) {
        std::string const frame = morph::net::detail::encodeWsFrame(opcode, payload, /*mask=*/false);
        _socket.sendAll(frame.data(), frame.size());
    }

    morph::net::detail::WsFrame receiveFrame() {
        for (;;) {
            if (auto frame = _reader.tryExtractFrame()) {
                return std::move(*frame);
            }
            char buf[4096];
            std::size_t const got = _socket.recvSome(buf, sizeof(buf));
            if (got == 0) {
                throw std::runtime_error("FakeWsServer::receiveFrame: peer closed");
            }
            _reader.feed(std::string_view{buf, got});
        }
    }

    // Reads frames until a Text one arrives and decodes it as an Envelope --
    // for tests that need to answer a specific request (e.g. by its callId).
    morph::wire::Envelope receiveEnvelope() {
        for (;;) {
            auto frame = receiveFrame();
            if (frame.opcode == morph::net::detail::WsOpcode::kText) {
                return morph::wire::decode(frame.payload);
            }
        }
    }

    // Tears the connection down with an abortive RST (SO_LINGER{1,0}) instead
    // of a normal FIN -- for tests aiming at a socket-error window rather than
    // a clean peer-closed one.
    void closeAbruptly() {
        struct linger l{};
        l.l_onoff = 1;
        l.l_linger = 0;
        ::setsockopt(_socket.nativeHandle(), SOL_SOCKET, SO_LINGER, &l, sizeof(l));
        _socket = morph::net::detail::TcpSocket{};
    }

private:
    morph::net::detail::TcpSocket _listener;
    morph::net::detail::TcpSocket _socket;
    morph::net::detail::WsFrameReader _reader;
};

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

TEST_CASE("SocketBackend: two clients sharing a key reach one instance over the wire",
          "[net][socket_backend][shared-instances]") {
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
    morph::bridge::BridgeHandler<SbCounterModel, morph::bridge::AllowShared> fromA{bridgeA, &cbPool};
    morph::bridge::BridgeHandler<SbCounterModel, morph::bridge::AllowShared> fromB{bridgeB, &cbPool};

    // Two genuinely separate clients, two sockets, one server-side directory.
    std::atomic<int> lastA{-1};
    fromA.execute(SbBump{.id = 77, .by = 10})
        .then([&](const SbTotal& res) { lastA.store(res.value); })
        .onError([](const std::exception_ptr&) {});
    spinUntil([&] { return lastA.load() != -1; });
    REQUIRE(lastA.load() == 10);

    std::atomic<int> lastB{-1};
    fromB.execute(SbBump{.id = 77, .by = 5})
        .then([&](const SbTotal& res) { lastB.store(res.value); })
        .onError([](const std::exception_ptr&) {});
    spinUntil([&] { return lastB.load() != -1; });
    // 15, not 5: the second client attached to the first client's instance.
    REQUIRE(lastB.load() == 15);

    std::atomic<int> keyCount{-1};
    fromB.instances()
        .then([&](const std::vector<std::int64_t>& keys) { keyCount.store(static_cast<int>(keys.size())); })
        .onError([](const std::exception_ptr&) {});
    spinUntil([&] { return keyCount.load() != -1; });
    REQUIRE(keyCount.load() == 1);
}

TEST_CASE("SocketBackend: a plain handler keeps its own instance over the wire", "[net][socket_backend]") {
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    morph::net::SocketServer wsServer{*server, 0};
    REQUIRE(wsServer.listen());

    std::string const url = "ws://127.0.0.1:" + std::to_string(static_cast<unsigned>(wsServer.port()));
    auto shared = std::make_unique<morph::net::SocketBackend>(url);
    auto priv = std::make_unique<morph::net::SocketBackend>(url);
    REQUIRE(shared->waitForConnected());
    REQUIRE(priv->waitForConnected());

    morph::exec::ThreadPoolExecutor cbPool{2};
    morph::bridge::Bridge sharedBridge{std::move(shared)};
    morph::bridge::Bridge privBridge{std::move(priv)};
    morph::bridge::BridgeHandler<SbCounterModel, morph::bridge::AllowShared> joined{sharedBridge, &cbPool};
    morph::bridge::BridgeHandler<SbCounterModel> alone{privBridge, &cbPool};

    std::atomic<int> lastShared{-1};
    joined.execute(SbBump{.id = 88, .by = 30})
        .then([&](const SbTotal& res) { lastShared.store(res.value); })
        .onError([](const std::exception_ptr&) {});
    spinUntil([&] { return lastShared.load() != -1; });
    REQUIRE(lastShared.load() == 30);

    std::atomic<int> lastPriv{-1};
    alone.execute(SbBump{.id = 88, .by = 1})
        .then([&](const SbTotal& res) { lastPriv.store(res.value); })
        .onError([](const std::exception_ptr&) {});
    spinUntil([&] { return lastPriv.load() != -1; });
    // Opted out, so it registered its own instance and counts from zero.
    REQUIRE(lastPriv.load() == 1);
}

TEST_CASE("SocketBackend: a fire-and-forget deregister's reply is not consumed by a parked sync call",
          "[net][socket_backend]") {
    // Regression coverage for morph#454 -- morph#65 reintroduced in this
    // transport. `deregisterModel` sends fire-and-forget, but the server still
    // answers it with an `ok` (remote.hpp's deregister branch), and that reply
    // carries whatever `callId` the request had. With `callId == 0` -- the
    // sentinel `dispatchIncomingEnvelope` reads as "hand this payload to
    // whichever sendSync() is parked" -- the deregister's own stray `ok` was
    // handed to the *next* synchronous control call instead of that call's
    // real reply.
    //
    // `attachModel`'s private-handoff branch is the shortest path to the
    // collision: with an empty primary it deregisters `current` and then
    // immediately registers a fresh instance over the same connection, so the
    // register parks on `_syncCv` with the deregister's reply already in
    // flight ahead of its own. An `ok` for a deregister carries `modelId ==
    // 0`, so the register returned `ModelId{0}` -- while the server had in
    // fact created the instance, which then leaked until the connection
    // closed.
    //
    // The assertion has to be that the returned id is a *real, different* id:
    // "attachModel did not throw" holds with the bug present, and so does "an
    // id came back" if 0 is allowed to count as one.
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    morph::net::SocketServer wsServer{*server, 0};
    REQUIRE(wsServer.listen());

    std::string const url = "ws://127.0.0.1:" + std::to_string(static_cast<unsigned>(wsServer.port()));
    morph::net::SocketBackend backend{url};
    REQUIRE(backend.waitForConnected());

    auto const mid1 = backend.registerModel("SbEchoModel", nullptr);
    REQUIRE(mid1.v != 0U);

    // Empty primary => the private-handoff branch: deregister(mid1) then
    // register, back to back on one connection.
    auto const mid2 = backend.attachModel("SbEchoModel", nullptr, {}, mid1);
    REQUIRE(mid2.v != 0U);
    REQUIRE(mid2.v != mid1.v);

    // …and the id handed back has to be one the server actually holds, which
    // is what proves the reply that woke the register was the register's own
    // rather than some other message's that happened to carry an id.
    // Hand-built ActionCall (rather than a BridgeHandler) so the backend's
    // own attachModel result is the thing under test; the raw reply body is
    // kept as a string so this test needs no JSON dependency of its own.
    morph::backend::detail::ActionCall call{
        .modelTypeId = "SbEchoModel",
        .actionTypeId = "SbEchoAction",
        .serializeAction = [] { return std::string{R"({"value":7})"}; },
        .deserializeResult =
            [](std::string_view body) { return std::static_pointer_cast<void>(std::make_shared<std::string>(body)); },
        .localOp = nullptr,
        .session = {},
    };

    morph::exec::ThreadPoolExecutor cbPool{1};
    std::atomic<bool> settled{false};
    std::string echoed;
    backend.execute(mid2, std::move(call), &cbPool)
        .then([&](const std::shared_ptr<void>& res) {
            echoed = *std::static_pointer_cast<std::string>(res);
            settled.store(true);
        })
        .onError([&](const std::exception_ptr&) { settled.store(true); });
    spinUntil([&] { return settled.load(); });
    REQUIRE(echoed == "7");
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

TEST_CASE("SocketBackend: registerModel surfaces the server's error reply for an unknown type",
          "[net][socket_backend]") {
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    morph::net::SocketServer wsServer{*server, 0};
    REQUIRE(wsServer.listen());

    std::string const url = "ws://127.0.0.1:" + std::to_string(static_cast<unsigned>(wsServer.port()));
    morph::net::SocketBackend backend{url};
    REQUIRE(backend.waitForConnected());

    REQUIRE_THROWS_WITH(backend.registerModel("NoSuchModelTypeId", nullptr),
                        Catch::Matchers::ContainsSubstring("register failed"));
}

TEST_CASE("SocketBackend: registerModelShared with an empty primary degrades to a private register",
          "[net][socket_backend]") {
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    morph::net::SocketServer wsServer{*server, 0};
    REQUIRE(wsServer.listen());

    std::string const url = "ws://127.0.0.1:" + std::to_string(static_cast<unsigned>(wsServer.port()));
    morph::net::SocketBackend backend{url};
    REQUIRE(backend.waitForConnected());

    auto mid = backend.registerModelShared("SbEchoModel", nullptr, morph::backend::detail::InstanceIdentity{});
    REQUIRE(mid.v != 0U);
}

TEST_CASE("SocketBackend: registerModelShared with a primary reaches the server's register-or-attach directory",
          "[net][socket_backend][shared-instances]") {
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    morph::net::SocketServer wsServer{*server, 0};
    REQUIRE(wsServer.listen());

    std::string const url = "ws://127.0.0.1:" + std::to_string(static_cast<unsigned>(wsServer.port()));
    morph::net::SocketBackend backendA{url};
    morph::net::SocketBackend backendB{url};
    REQUIRE(backendA.waitForConnected());
    REQUIRE(backendB.waitForConnected());

    auto midA = backendA.registerModelShared("SbCounterModel", nullptr,
                                             morph::backend::detail::InstanceIdentity{.primary = "sb-shared-key-1"});
    REQUIRE(midA.v != 0U);
    // The second caller naming the same primary reaches the same instance
    // rather than creating a new one -- confirms this actually went through
    // the server's shared directory, not just that some instance came back.
    auto midB = backendB.registerModelShared("SbCounterModel", nullptr,
                                             morph::backend::detail::InstanceIdentity{.primary = "sb-shared-key-1"});
    REQUIRE(midB.v == midA.v);
}

TEST_CASE("SocketBackend: attachModel with an empty primary and current==0 registers a private instance",
          "[net][socket_backend]") {
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    morph::net::SocketServer wsServer{*server, 0};
    REQUIRE(wsServer.listen());

    std::string const url = "ws://127.0.0.1:" + std::to_string(static_cast<unsigned>(wsServer.port()));
    morph::net::SocketBackend backend{url};
    REQUIRE(backend.waitForConnected());

    // current == 0: no prior instance to give up, so this is just a private
    // register via registerModelWithContext.
    auto mid = backend.attachModel("SbEchoModel", nullptr, morph::backend::detail::InstanceIdentity{},
                                   morph::exec::detail::ModelId{0});
    REQUIRE(mid.v != 0U);
}

TEST_CASE("SocketBackend: attachModel's empty-primary path deregisters the instance being given up",
          "[net][socket_backend]") {
    // Also exercises (and documents) a known cross-talk hazard, filed as
    // morph#454: deregisterModel()'s fire-and-forget server acknowledgment
    // and a synchronous control call's reply both travel as callId == 0, so
    // a synchronous call issued immediately after a deregister -- exactly
    // what this branch does (`deregisterModel(current)` followed immediately
    // by `registerModelWithContext(...)`) -- can observe the deregister's own
    // stray "ok" instead of its own reply. That corrupts the *client-side
    // return value* of the private re-register below on this machine most
    // runs, which is why it is deliberately not asserted on here. What *is*
    // asserted -- the server-side release of the instance being given up --
    // is unaffected by which reply the client happened to decode.
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    morph::net::SocketServer wsServer{*server, 0};
    REQUIRE(wsServer.listen());

    std::string const url = "ws://127.0.0.1:" + std::to_string(static_cast<unsigned>(wsServer.port()));
    morph::net::SocketBackend backend{url};
    REQUIRE(backend.waitForConnected());

    // A *shared* instance, so its release is externally observable via
    // listInstances -- unlike a private one, which never enters the directory.
    auto shared = backend.registerModelShared("SbCounterModel", nullptr,
                                              morph::backend::detail::InstanceIdentity{.primary = "handoff-key"});
    REQUIRE(shared.v != 0U);

    // current != 0, empty primary: the private-handoff path -- give up the
    // shared instance for a fresh private one. Must not throw or hang even
    // though the reply it decodes may be the deregister's stray ack (#454).
    REQUIRE_NOTHROW(
        backend.attachModel("SbCounterModel", nullptr, morph::backend::detail::InstanceIdentity{}, shared));

    // Let the real (now-orphaned) register reply this call's sendSync did not
    // consume finish draining before starting a fresh synchronous call below
    // -- otherwise it could itself be misdelivered to that call by the same
    // #454 hazard, corrupting *this* test's own verification step.
    std::this_thread::sleep_for(std::chrono::milliseconds{100});

    // The instance held under "handoff-key" must be released -- confirms
    // deregisterModel(current) genuinely ran (the server-side effect, which
    // #454 does not touch).
    bool released = false;
    for (int i = 0; i < 100 && !released; ++i) {
        auto keys = backend.listInstances("SbCounterModel");
        released = std::find(keys.begin(), keys.end(), "handoff-key") == keys.end();
        if (!released) {
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
    }
    REQUIRE(released);
}

TEST_CASE("SocketBackend: assignPrimary is a no-op guard for an empty primary or a zero ModelId",
          "[net][socket_backend]") {
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    morph::net::SocketServer wsServer{*server, 0};
    REQUIRE(wsServer.listen());

    std::string const url = "ws://127.0.0.1:" + std::to_string(static_cast<unsigned>(wsServer.port()));
    morph::net::SocketBackend backend{url};
    REQUIRE(backend.waitForConnected());

    auto mid = backend.registerModel("SbCounterModel", nullptr);
    REQUIRE_NOTHROW(backend.assignPrimary(mid, "SbCounterModel", ""));
    REQUIRE_NOTHROW(backend.assignPrimary(morph::exec::detail::ModelId{0}, "SbCounterModel", "unused-key"));
}

TEST_CASE("SocketBackend: assignPrimary files a live instance a second client can then attach to",
          "[net][socket_backend][shared-instances]") {
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    morph::net::SocketServer wsServer{*server, 0};
    REQUIRE(wsServer.listen());

    std::string const url = "ws://127.0.0.1:" + std::to_string(static_cast<unsigned>(wsServer.port()));
    morph::net::SocketBackend backendA{url};
    morph::net::SocketBackend backendB{url};
    REQUIRE(backendA.waitForConnected());
    REQUIRE(backendB.waitForConnected());

    auto midA = backendA.registerModel("SbCounterModel", nullptr);
    REQUIRE(midA.v != 0U);
    REQUIRE_NOTHROW(backendA.assignPrimary(midA, "SbCounterModel", "sb-assigned-key-1"));

    auto midB = backendB.attachModel("SbCounterModel", nullptr,
                                     morph::backend::detail::InstanceIdentity{.primary = "sb-assigned-key-1"},
                                     morph::exec::detail::ModelId{0});
    REQUIRE(midB.v == midA.v);
}

TEST_CASE("SocketBackend: listInstances surfaces the server's error reply when unauthorized",
          "[net][socket_backend]") {
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto authz = std::make_shared<DenyAllAuthorizer>();
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool, authz);
    morph::net::SocketServer wsServer{*server, 0};
    REQUIRE(wsServer.listen());

    std::string const url = "ws://127.0.0.1:" + std::to_string(static_cast<unsigned>(wsServer.port()));
    morph::net::SocketBackend backend{url};
    REQUIRE(backend.waitForConnected());

    REQUIRE_THROWS_WITH(backend.listInstances("SbCounterModel"),
                        Catch::Matchers::ContainsSubstring("instances failed"));
}

TEST_CASE("SocketBackend: listInstances throws when the server's ok reply body fails to decode",
          "[net][socket_backend][fault-injection]") {
    FakeWsServer fake;
    morph::net::SocketBackend backend{"ws://127.0.0.1:" + std::to_string(static_cast<unsigned>(fake.port()))};
    fake.acceptAndHandshake();
    REQUIRE(backend.waitForConnected());

    std::thread serverThread{[&] {
        auto env = fake.receiveEnvelope();
        fake.sendFrame(morph::net::detail::WsOpcode::kText,
                       morph::wire::encode(morph::wire::makeOk(env.callId, "not-a-json-array")));
    }};

    REQUIRE_THROWS_WITH(backend.listInstances("SbEchoModel"),
                        Catch::Matchers::ContainsSubstring("instances decode failed"));
    serverThread.join();
}

TEST_CASE("SocketBackend: listInstances on a disconnected socket throws instead of hanging",
          "[net][socket_backend][disconnect]") {
    morph::net::SocketBackend backend{"ws://127.0.0.1:1"};
    REQUIRE_FALSE(backend.waitForConnected(std::chrono::milliseconds{200}));
    REQUIRE_THROWS_WITH(backend.listInstances("SbEchoModel"), Catch::Matchers::ContainsSubstring("instances failed"));
}

TEST_CASE("SocketBackend: attachModel surfaces the server's error reply when registration is unauthorized",
          "[net][socket_backend]") {
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto authz = std::make_shared<DenyAllAuthorizer>();
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool, authz);
    morph::net::SocketServer wsServer{*server, 0};
    REQUIRE(wsServer.listen());

    std::string const url = "ws://127.0.0.1:" + std::to_string(static_cast<unsigned>(wsServer.port()));
    morph::net::SocketBackend backend{url};
    REQUIRE(backend.waitForConnected());

    REQUIRE_THROWS_WITH(
        backend.attachModel("SbEchoModel", nullptr, morph::backend::detail::InstanceIdentity{.primary = "k1"},
                            morph::exec::detail::ModelId{0}),
        Catch::Matchers::ContainsSubstring("attach failed"));
}

TEST_CASE("SocketBackend: attachModel on a disconnected socket throws instead of hanging",
          "[net][socket_backend][disconnect]") {
    morph::net::SocketBackend backend{"ws://127.0.0.1:1"};
    REQUIRE_FALSE(backend.waitForConnected(std::chrono::milliseconds{200}));
    REQUIRE_THROWS_WITH(
        backend.attachModel("SbEchoModel", nullptr, morph::backend::detail::InstanceIdentity{.primary = "k1"},
                            morph::exec::detail::ModelId{0}),
        Catch::Matchers::ContainsSubstring("attach failed"));
}

TEST_CASE("morph::net::SocketBackend::notifyBackendChanged is a documented no-op", "[net][socket_backend]") {
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    morph::net::SocketServer wsServer{*server, 0};
    REQUIRE(wsServer.listen());

    std::string const url = "ws://127.0.0.1:" + std::to_string(static_cast<unsigned>(wsServer.port()));
    morph::net::SocketBackend backend{url};
    REQUIRE(backend.waitForConnected());

    REQUIRE_NOTHROW(backend.notifyBackendChanged());
}

TEST_CASE("SocketBackend: a second synchronous call while one is in flight throws the reentrant-use error",
          "[net][socket_backend]") {
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto authz = std::make_shared<SlowAuthorizer>(std::chrono::milliseconds{150});
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool, authz);
    morph::net::SocketServer wsServer{*server, 0};
    REQUIRE(wsServer.listen());

    std::string const url = "ws://127.0.0.1:" + std::to_string(static_cast<unsigned>(wsServer.port()));
    morph::net::SocketBackend backend{url};
    REQUIRE(backend.waitForConnected());

    std::atomic<int> reentrantErrors{0};
    std::atomic<int> succeeded{0};
    auto attempt = [&] {
        try {
            (void)backend.registerModel("SbEchoModel", nullptr);
            succeeded.fetch_add(1);
        } catch (const std::exception& exc) {
            if (std::string{exc.what()}.find("reentrant") != std::string::npos) {
                reentrantErrors.fetch_add(1);
            }
        }
    };
    std::thread t1{attempt};
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    std::thread t2{attempt};
    t1.join();
    t2.join();

    REQUIRE(reentrantErrors.load() == 1);
    REQUIRE(succeeded.load() == 1);
}

TEST_CASE("SocketBackend: server dropping while a synchronous call is genuinely in flight throws disconnected",
          "[net][socket_backend][disconnect]") {
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto authz = std::make_shared<SlowAuthorizer>(std::chrono::milliseconds{300});
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool, authz);
    auto wsServer = std::make_unique<morph::net::SocketServer>(*server, 0);
    REQUIRE(wsServer->listen());

    std::string const url = "ws://127.0.0.1:" + std::to_string(static_cast<unsigned>(wsServer->port()));
    morph::net::SocketBackend backend{url};
    REQUIRE(backend.waitForConnected());

    bool threw = false;
    std::string what;
    std::thread caller{[&] {
        try {
            (void)backend.registerModel("SbEchoModel", nullptr);
        } catch (const std::exception& exc) {
            threw = true;
            what = exc.what();
        }
    }};
    // authorizeRegister() is parked mid-sleep on the server's worker pool at
    // this point -- the client is genuinely blocked in sendSync's _syncCv.wait,
    // not merely about to call it.
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    wsServer->close();
    wsServer.reset();
    caller.join();

    REQUIRE(threw);
    REQUIRE(what.find("register failed") != std::string::npos);
    REQUIRE(what.find("disconnected") != std::string::npos);
}

TEST_CASE("SocketBackend: an undecodable message from the server with no sync call in flight fails pending executes",
          "[net][socket_backend][fault-injection]") {
    FakeWsServer fake;
    auto backendPtr = std::make_unique<morph::net::SocketBackend>("ws://127.0.0.1:" +
                                                                  std::to_string(static_cast<unsigned>(fake.port())));
    fake.acceptAndHandshake();
    REQUIRE(backendPtr->waitForConnected());

    morph::backend::detail::ActionCall call;
    call.modelTypeId = "SbEchoModel";
    call.actionTypeId = "SbEchoAction";
    call.serializeAction = [] { return std::string{"{}"}; };
    call.deserializeResult = [](std::string_view) -> std::shared_ptr<void> { return nullptr; };

    // Drain the outgoing "execute" request off the wire first, so the pending
    // entry genuinely exists server-side before the garbage reply lands.
    std::thread serverThread{[&] { (void)fake.receiveEnvelope(); }};
    morph::exec::ThreadPoolExecutor cbPool{1};
    auto comp = backendPtr->execute(morph::exec::detail::ModelId{1}, std::move(call), &cbPool);
    serverThread.join();

    std::atomic<bool> gotProtocolError{false};
    comp.onError([&](const std::exception_ptr& exc) {
        try {
            std::rethrow_exception(exc);
        } catch (const std::exception& e) {
            if (std::string{e.what()}.find("protocol error") != std::string::npos) {
                gotProtocolError.store(true);
            }
        }
    });

    fake.sendFrame(morph::net::detail::WsOpcode::kText, "this is not a valid envelope");

    spinUntil([&] { return gotProtocolError.load(); });
    REQUIRE(gotProtocolError.load());
}

TEST_CASE(
    "SocketBackend: an undecodable message from the server while a synchronous call is in flight is handed to it",
    "[net][socket_backend][fault-injection]") {
    FakeWsServer fake;
    morph::net::SocketBackend backend{"ws://127.0.0.1:" + std::to_string(static_cast<unsigned>(fake.port()))};
    fake.acceptAndHandshake();
    REQUIRE(backend.waitForConnected());

    std::thread serverThread{[&] {
        (void)fake.receiveEnvelope();  // the register request
        fake.sendFrame(morph::net::detail::WsOpcode::kText, "this is not a valid envelope either");
    }};

    // registerModel's own wire::decode(replyJson) throws on the garbage handed
    // back by dispatchIncomingEnvelope's _syncInFlight branch -- any exception
    // confirms the parked sendSync woke with that payload instead of hanging
    // or (incorrectly) routing it through cancelPending.
    REQUIRE_THROWS(backend.registerModel("SbEchoModel", nullptr));
    serverThread.join();
}

TEST_CASE("SocketBackend: an execute reply with an unrecognized callId from the server is dropped silently",
          "[net][socket_backend][fault-injection]") {
    FakeWsServer fake;
    morph::net::SocketBackend backend{"ws://127.0.0.1:" + std::to_string(static_cast<unsigned>(fake.port()))};
    fake.acceptAndHandshake();
    REQUIRE(backend.waitForConnected());

    // A reply naming a callId the client never issued -- e.g. late after the
    // original caller already gave up, or a duplicate -- must be dropped
    // silently rather than crash or corrupt any other pending call.
    fake.sendFrame(morph::net::detail::WsOpcode::kText, morph::wire::encode(morph::wire::makeOk(999999, "42")));

    morph::backend::detail::ActionCall call;
    call.modelTypeId = "SbEchoModel";
    call.actionTypeId = "SbEchoAction";
    call.serializeAction = [] { return std::string{"{}"}; };
    call.deserializeResult = [](std::string_view body) -> std::shared_ptr<void> {
        return std::make_shared<int>(std::stoi(std::string{body}));
    };

    std::thread serverThread{[&] {
        auto env = fake.receiveEnvelope();
        fake.sendFrame(morph::net::detail::WsOpcode::kText, morph::wire::encode(morph::wire::makeOk(env.callId, "7")));
    }};

    morph::exec::ThreadPoolExecutor cbPool{1};
    std::atomic<int> result{-1};
    auto comp = backend.execute(morph::exec::detail::ModelId{1}, std::move(call), &cbPool);
    comp.then([&](const std::shared_ptr<void>& v) { result.store(*std::static_pointer_cast<int>(v)); })
        .onError([](const std::exception_ptr&) {});

    spinUntil([&] { return result.load() != -1; });
    REQUIRE(result.load() == 7);
    serverThread.join();
}

TEST_CASE("SocketBackend: execute resolves with an exception when the server's ok reply body fails to deserialize",
          "[net][socket_backend][fault-injection]") {
    FakeWsServer fake;
    morph::net::SocketBackend backend{"ws://127.0.0.1:" + std::to_string(static_cast<unsigned>(fake.port()))};
    fake.acceptAndHandshake();
    REQUIRE(backend.waitForConnected());

    morph::backend::detail::ActionCall call;
    call.modelTypeId = "SbEchoModel";
    call.actionTypeId = "SbEchoAction";
    call.serializeAction = [] { return std::string{"{}"}; };
    call.deserializeResult = [](std::string_view body) -> std::shared_ptr<void> {
        return std::make_shared<int>(std::stoi(std::string{body}));  // throws on non-numeric garbage
    };

    std::thread serverThread{[&] {
        auto env = fake.receiveEnvelope();
        fake.sendFrame(morph::net::detail::WsOpcode::kText,
                       morph::wire::encode(morph::wire::makeOk(env.callId, "not-a-number")));
    }};

    morph::exec::ThreadPoolExecutor cbPool{1};
    std::atomic<bool> gotError{false};
    auto comp = backend.execute(morph::exec::detail::ModelId{1}, std::move(call), &cbPool);
    comp.then([](const std::shared_ptr<void>&) {}).onError([&](const std::exception_ptr&) { gotError.store(true); });

    spinUntil([&] { return gotError.load(); });
    REQUIRE(gotError.load());
    serverThread.join();
}

TEST_CASE("SocketBackend: a malformed WebSocket frame from the server is treated as a disconnect",
          "[net][socket_backend][fault-injection]") {
    FakeWsServer fake;
    morph::net::SocketBackend backend{"ws://127.0.0.1:" + std::to_string(static_cast<unsigned>(fake.port()))};
    fake.acceptAndHandshake();
    REQUIRE(backend.waitForConnected());

    // A continuation frame with no message in progress is a protocol
    // violation WsFrameReader::tryExtractFrame() rejects outright -- the same
    // violation test_socket_server.cpp uses for its own mirror-image finding
    // (a malformed frame arriving from the *client*).
    fake.sendFrame(morph::net::detail::WsOpcode::kContinuation, "");

    REQUIRE(waitForDisconnect(backend));
}

TEST_CASE("SocketBackend: a Close frame from the server is echoed and ends the connection",
          "[net][socket_backend][fault-injection]") {
    FakeWsServer fake;
    morph::net::SocketBackend backend{"ws://127.0.0.1:" + std::to_string(static_cast<unsigned>(fake.port()))};
    fake.acceptAndHandshake();
    REQUIRE(backend.waitForConnected());

    fake.sendFrame(morph::net::detail::WsOpcode::kClose, "");
    auto echoed = fake.receiveFrame();
    REQUIRE(echoed.opcode == morph::net::detail::WsOpcode::kClose);

    REQUIRE(waitForDisconnect(backend));
}

TEST_CASE("SocketBackend: a Ping frame from the server is answered with a matching Pong",
          "[net][socket_backend][fault-injection]") {
    FakeWsServer fake;
    morph::net::SocketBackend backend{"ws://127.0.0.1:" + std::to_string(static_cast<unsigned>(fake.port()))};
    fake.acceptAndHandshake();
    REQUIRE(backend.waitForConnected());

    fake.sendFrame(morph::net::detail::WsOpcode::kPing, "ping-payload");
    auto pong = fake.receiveFrame();
    REQUIRE(pong.opcode == morph::net::detail::WsOpcode::kPong);
    REQUIRE(pong.payload == "ping-payload");

    // The read loop kept going afterward -- a second Ping still gets answered.
    fake.sendFrame(morph::net::detail::WsOpcode::kPing, "ping-payload-2");
    auto pong2 = fake.receiveFrame();
    REQUIRE(pong2.opcode == morph::net::detail::WsOpcode::kPong);
    REQUIRE(pong2.payload == "ping-payload-2");
}

TEST_CASE("SocketBackend: a Ping frame followed by an abortive close does not hang the io thread",
          "[net][socket_backend][disconnect]") {
    // Aims at drainFrames()'s own Ping-echo `sendFrame(kPong, ...)` catch
    // (distinct from -- and narrower than -- the general sendFrame-races-a-
    // disconnect family closed above): here the *same* io thread reads the
    // Ping and then, moments later, tries to write the Pong back on a socket
    // an abortive RST may have already torn down. The RST and the read+echo
    // sequence race each other with no synchronization, so whether this
    // specific catch fires depends on exactly how fast the RST lands versus
    // how fast the io thread turns the Ping around -- not reliably won on
    // every run. What *is* guaranteed regardless of who wins: the io thread
    // does not hang, and the backend ends up disconnected either way.
    for (int iter = 0; iter < 40; ++iter) {
        FakeWsServer fake;
        morph::net::SocketBackend backend{"ws://127.0.0.1:" + std::to_string(static_cast<unsigned>(fake.port()))};
        fake.acceptAndHandshake();
        REQUIRE(backend.waitForConnected());

        fake.sendFrame(morph::net::detail::WsOpcode::kPing, "racing-the-rst");
        fake.closeAbruptly();

        REQUIRE(waitForDisconnect(backend, 25));
    }
}

TEST_CASE("SocketBackend: a Close frame followed by an abortive close does not hang the io thread",
          "[net][socket_backend][disconnect]") {
    // Same race as the Ping case above, aimed at drainFrames()'s Close-echo
    // `sendFrame(kClose, "")` catch instead of the Ping one.
    for (int iter = 0; iter < 40; ++iter) {
        FakeWsServer fake;
        morph::net::SocketBackend backend{"ws://127.0.0.1:" + std::to_string(static_cast<unsigned>(fake.port()))};
        fake.acceptAndHandshake();
        REQUIRE(backend.waitForConnected());

        fake.sendFrame(morph::net::detail::WsOpcode::kClose, "");
        fake.closeAbruptly();

        REQUIRE(waitForDisconnect(backend, 25));
    }
}

TEST_CASE("SocketBackend: an unsolicited Pong and a Binary frame from the server are silently ignored",
          "[net][socket_backend][fault-injection]") {
    FakeWsServer fake;
    morph::net::SocketBackend backend{"ws://127.0.0.1:" + std::to_string(static_cast<unsigned>(fake.port()))};
    fake.acceptAndHandshake();
    REQUIRE(backend.waitForConnected());

    fake.sendFrame(morph::net::detail::WsOpcode::kPong, "unsolicited");
    fake.sendFrame(morph::net::detail::WsOpcode::kBinary, "binary-payload");

    // Neither elicits any reply nor breaks the connection -- prove it with a
    // Ping/Pong round-trip afterward.
    fake.sendFrame(morph::net::detail::WsOpcode::kPing, "still-alive");
    auto pong = fake.receiveFrame();
    REQUIRE(pong.opcode == morph::net::detail::WsOpcode::kPong);
    REQUIRE(pong.payload == "still-alive");
}

TEST_CASE("SocketBackend: with reconnectEnabled=false, the backend does not retry after a disconnect",
          "[net][socket_backend][disconnect]") {
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);

    morph::net::SocketBackend::Config cfg;
    cfg.reconnectEnabled = false;

    auto wsServer = std::make_unique<morph::net::SocketServer>(*server, 0);
    REQUIRE(wsServer->listen());
    std::uint16_t const port = wsServer->port();
    std::string const url = "ws://127.0.0.1:" + std::to_string(static_cast<unsigned>(port));

    morph::net::SocketBackend backend{url, cfg};
    REQUIRE(backend.waitForConnected());
    wsServer.reset();  // drop the connection

    REQUIRE(waitForDisconnect(backend));

    // A fresh listener on the same port must not bring the backend back --
    // with reconnectEnabled=false, the io thread already returned for good.
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    auto wsServer2 = std::make_unique<morph::net::SocketServer>(*server, port);
    REQUIRE(wsServer2->listen());
    std::this_thread::sleep_for(std::chrono::milliseconds{300});
    REQUIRE_FALSE(backend.waitForConnected(std::chrono::milliseconds{100}));
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

TEST_CASE("SocketBackend: execute() racing a disconnect never leaves a Completion unresolved",
          "[net][socket_backend][disconnect]") {
    // Targets the exact TOCTOU window execute()'s _pendingMtx re-check of
    // _connected closes (see that function's own comment): onDisconnected()
    // sets _connected = false and then sweeps _pending, both under
    // _pendingMtx. Before the fix, execute() checked _connected once, *before*
    // taking that lock; a disconnect's sweep landing strictly between that
    // check and the _pending insert drained a table the about-to-be-inserted
    // entry had not joined yet, leaving that one Completion permanently
    // unresolved -- a silent hang, not a crash.
    //
    // The window is a handful of instructions wide and cannot be hit
    // deterministically without a test-only hook this header does not have.
    // A modest handful of concurrent execute() calls against a connection
    // dropped mid-stream, repeated over a few iterations on fresh sockets
    // each time, drives the same race statistically instead: every run below
    // either the fix holds on every call it happens to interleave with, or
    // it doesn't and this test hangs (turned into a failure by ctest's
    // per-test TIMEOUT), exactly the failure mode a single well-aimed hit
    // would also produce.
    //
    // Deliberately NOT a heavier stress shape (many threads x many calls).
    // That shape belongs to morph#449 -- a stranded execute-ordering ticket
    // when a connection with several executes in flight drops -- whose own
    // hang would masquerade as a failure of *this* fix instead of the
    // ticket-ordering issue it actually is. morph#449 is fixed (see
    // `releaseExecuteTicket` in remote.hpp), and its own stress-shaped
    // regression test is "many concurrent executes racing a disconnect leave
    // no stranded execute ticket" below; the two are kept separate so a
    // regression in either one fails where it is diagnosed.
    for (int iter = 0; iter < 3; ++iter) {
        morph::exec::ThreadPoolExecutor serverPool{2};
        auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
        auto wsServer = std::make_unique<morph::net::SocketServer>(*server, 0);
        REQUIRE(wsServer->listen());

        std::string const url = "ws://127.0.0.1:" + std::to_string(static_cast<unsigned>(wsServer->port()));
        auto backendPtr = std::make_unique<morph::net::SocketBackend>(url);
        REQUIRE(backendPtr->waitForConnected());

        morph::exec::ThreadPoolExecutor cbPool{2};
        morph::bridge::Bridge bridge{std::move(backendPtr)};
        morph::bridge::BridgeHandler<SbEchoModel> handler{bridge, &cbPool};

        constexpr int callsPerThread = 3;
        constexpr int producerThreads = 2;
        std::atomic<int> settled{0};
        std::vector<std::thread> producers;
        producers.reserve(producerThreads);
        for (int t = 0; t < producerThreads; ++t) {
            producers.emplace_back([&] {
                for (int i = 0; i < callsPerThread; ++i) {
                    handler.execute(SbEchoAction{i})
                        .then([&](int) { settled.fetch_add(1); })
                        .onError([&](const std::exception_ptr&) { settled.fetch_add(1); });
                }
            });
        }

        // Drop the connection while calls are still being issued and in
        // flight, aiming squarely at the window between "still connected"
        // and "the entry is in _pending".
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
        wsServer->close();
        wsServer.reset();

        for (auto& producer : producers) {
            producer.join();
        }

        // Every call this iteration issued must have settled -- as either a
        // real reply or DisconnectedError, never neither. spinUntil's own
        // bounded retry count (200 * 10ms = 2s) is what turns a genuine
        // regression into a REQUIRE failure instead of an indefinite hang.
        int const totalCalls = producerThreads * callsPerThread;
        spinUntil([&] { return settled.load() == totalCalls; }, 200);
        REQUIRE(settled.load() == totalCalls);

        // Let the OS release this iteration's port and the thread pools
        // above finish tearing down before the next iteration opens a new
        // socket -- same reasoning as "reconnects to a fresh server on the
        // same port"'s own 100ms pause.
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }
}

TEST_CASE("SocketBackend: sendFrame-triggering calls racing a hard disconnect never hang or corrupt state",
          "[net][socket_backend][disconnect]") {
    // Targets the narrow TOCTOU windows between a caller-side "am I
    // connected" check (deregisterModel()'s _connected.load(), execute()'s
    // insertIf admit predicate, sendSync()'s own _connected.load()) and
    // sendFrame()'s separately-locked `_socket.valid()` check a moment later:
    // onDisconnected() sets `_connected = false` and only *then* resets
    // `_socket`, under `_socketMtx` (see that function's own comment), so a
    // concurrent caller can pass its own check and still lose the race to the
    // socket being torn down by the time it actually writes. Same statistical
    // technique already proven above ("execute() racing a disconnect never
    // leaves a Completion unresolved") and in socket_server.hpp's own
    // TOCTOU-window tests (findings #9/#11 there, which needed ~200
    // iterations) -- broadened across every sendFrame() call site at once,
    // since it is the same underlying window regardless of which caller hits
    // it.
    for (int iter = 0; iter < 80; ++iter) {
        morph::exec::ThreadPoolExecutor serverPool{2};
        auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
        auto wsServer = std::make_unique<morph::net::SocketServer>(*server, 0);
        REQUIRE(wsServer->listen());

        std::string const url = "ws://127.0.0.1:" + std::to_string(static_cast<unsigned>(wsServer->port()));
        morph::net::SocketBackend backend{url};
        REQUIRE(backend.waitForConnected());

        auto mid = backend.registerModel("SbEchoModel", nullptr);
        REQUIRE(mid.v != 0U);

        std::atomic<bool> stop{false};
        std::vector<std::thread> hammer;
        for (int t = 0; t < 3; ++t) {
            hammer.emplace_back([&] {
                while (!stop.load(std::memory_order_relaxed)) {
                    backend.deregisterModel(mid);
                    morph::backend::detail::ActionCall call;
                    call.modelTypeId = "SbEchoModel";
                    call.actionTypeId = "SbEchoAction";
                    call.serializeAction = [] { return std::string{"{}"}; };
                    call.deserializeResult = [](std::string_view) -> std::shared_ptr<void> { return nullptr; };
                    (void)backend.execute(mid, std::move(call), nullptr);
                    try {
                        (void)backend.registerModel("SbEchoModel", nullptr);
                    } catch (const std::exception&) {
                        // "reentrant" (racing a sibling for the single-flight
                        // sendSync slot), "disconnected", or success are all
                        // fine here -- only a hang or crash would be a failure.
                    }
                }
            });
        }

        std::this_thread::sleep_for(std::chrono::microseconds{300});
        wsServer->close();
        wsServer.reset();
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
        stop.store(true);
        for (auto& th : hammer) {
            th.join();
        }
    }
}

TEST_CASE("SocketBackend: executeTimeout surfaces as backend::TimeoutError, not a generic runtime_error",
          "[net][socket_backend][timeout]") {
    // Regression coverage for dispatchIncomingEnvelope's env.message ==
    // wire::kExecuteTimeoutMessage branch (added alongside the SqliteOfflineQueue
    // and bridge.hpp fixes in #447): a server-side LimitPolicy::executeTimeout
    // reply must resolve the Completion with backend::TimeoutError specifically,
    // the same type QtWebSocketBackend/SimulatedRemoteBackend give callers for
    // this case -- not the generic std::runtime_error the `else` branch below it
    // produces for an arbitrary `err` message. Mirrors
    // tests/test_limit_policy.cpp's SimulatedRemoteBackend analog of this same
    // scenario, over the real WebSocket transport instead.
    morph::exec::ThreadPoolExecutor serverPool{4};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    morph::backend::LimitPolicy policy;
    policy.executeTimeout = std::chrono::milliseconds{50};
    server->setLimitPolicy(policy);
    morph::net::SocketServer wsServer{*server, 0};
    REQUIRE(wsServer.listen());

    std::string const url = "ws://127.0.0.1:" + std::to_string(static_cast<unsigned>(wsServer.port()));
    auto backendPtr = std::make_unique<morph::net::SocketBackend>(url);
    REQUIRE(backendPtr->waitForConnected());

    morph::exec::ThreadPoolExecutor cbPool{1};
    morph::bridge::Bridge bridge{std::move(backendPtr)};
    // SbSlowModel's execute() sleeps 300ms unconditionally -- well past the
    // 50ms executeTimeout above, so the server's timeout scheduler fires
    // before the strand result ever comes back.
    morph::bridge::BridgeHandler<SbSlowModel> handler{bridge, &cbPool};

    std::atomic<bool> gotTimeoutError{false};
    std::atomic<bool> gotSomethingElse{false};
    handler.execute(SbSlowAction{5}).then([](int) {}).onError([&](const std::exception_ptr& exc) {
        try {
            std::rethrow_exception(exc);
        } catch (const morph::backend::TimeoutError&) {
            gotTimeoutError.store(true);
        } catch (...) {
            gotSomethingElse.store(true);
        }
    });

    spinUntil([&] { return gotTimeoutError.load() || gotSomethingElse.load(); });
    CHECK_FALSE(gotSomethingElse.load());
    REQUIRE(gotTimeoutError.load());
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

TEST_CASE("SocketBackend: a reconnect handler throwing a non-std::exception leaves the transport usable",
          "[net][socket_backend][disconnect]") {
    // Same shape as the std::exception case above, but the handler throws a
    // non-std::exception value (an int) instead, targeting
    // handlerThreadMain()'s separate `catch (...)` clause.
    ReconnectFixture fixture;
    std::atomic<int> handlerCalls{0};

    fixture.bounce([&] {
        handlerCalls.fetch_add(1);
        throw 42;  // NOLINT(hicpp-exception-baseclass) -- deliberately not a std::exception
    });

    spinUntil([&] { return handlerCalls.load() > 0; }, 500);
    CHECK(handlerCalls.load() > 0);
    CHECK(fixture.backend->registerModel("SbEchoModel", nullptr).v != 0U);
}

// The Catch2 assertion macros, not branching logic, are what push this over the
// cognitive-complexity threshold -- as in the sibling disconnect cases above.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("SocketBackend: many concurrent executes racing a disconnect leave no stranded execute ticket",
          "[net][socket_backend][disconnect]") {
    // Regression coverage for morph#449 at the transport level; the mechanism
    // itself is pinned deterministically by
    // tests/test_remote_execute_ordering.cpp's "an execute rejected out of
    // ticket order..." case. This is the shape that actually found it, kept
    // because it is the one that exercises the real teardown ordering:
    // dropping the connection reclaims that connection's models, so the
    // executes still in flight for one model split into some that find the
    // model and some that reject with "model not found" -- and a rejection
    // releases its execute-ordering ticket immediately, without waiting for
    // its turn. `releaseExecuteTicket` used to advance `nextToRun` past any
    // earlier ticket when that happened, leaving that ticket's waiter parked
    // in `awaitExecuteTurn` on a predicate that could never come true again.
    //
    // The visible failure is not this test's assertions: it is the teardown
    // below it. A stranded ticket holds a `serverPool` worker forever, so
    // `~ThreadPoolExecutor` hangs in join() at end of scope and the whole
    // binary stops -- turned into a failure by ctest's per-test TIMEOUT.
    //
    // Honest about what this test is and is not. It is a *probabilistic*
    // shape, and a shallow one: measured on Linux/clang 22 against the
    // pre-fix code it hung 4 times in 145 runs at these thread and call
    // counts (and 0 times in 20 at the lighter 3x15 shape the issue reports,
    // which is why the counts here are higher than the issue's). It is
    // therefore not the control for the fix -- the deterministic case named
    // above is, and it fails 100% of runs without it. This one is kept
    // because it is the only test that drives the real disconnect teardown
    // that produces the out-of-order release in the first place, and because
    // it costs ~0.2s.
    //
    // It is deliberately the "heavier stress shape" the sibling
    // "execute() racing a disconnect never leaves a Completion unresolved"
    // case above avoids: kept separate so a morph#449 regression fails here,
    // where it is diagnosed, rather than masquerading as a failure of that
    // test's own TOCTOU fix.
    for (int iter = 0; iter < 3; ++iter) {
        morph::exec::ThreadPoolExecutor serverPool{4};
        auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
        auto wsServer = std::make_unique<morph::net::SocketServer>(*server, 0);
        REQUIRE(wsServer->listen());

        std::string const url = "ws://127.0.0.1:" + std::to_string(static_cast<unsigned>(wsServer->port()));
        auto backendPtr = std::make_unique<morph::net::SocketBackend>(url);
        REQUIRE(backendPtr->waitForConnected());

        morph::exec::ThreadPoolExecutor cbPool{2};
        morph::bridge::Bridge bridge{std::move(backendPtr)};
        // One shared handler, so every call targets the *same* model id and
        // therefore the same execute-ordering gate -- the gate is per-model,
        // so calls spread across instances would not queue behind each other
        // at all.
        morph::bridge::BridgeHandler<SbEchoModel> handler{bridge, &cbPool};

        constexpr int callsPerThread = 60;
        constexpr int producerThreads = 8;
        std::atomic<int> settled{0};
        std::vector<std::thread> producers;
        producers.reserve(producerThreads);
        for (int producer = 0; producer < producerThreads; ++producer) {
            producers.emplace_back([&] {
                for (int i = 0; i < callsPerThread; ++i) {
                    handler.execute(SbEchoAction{i})
                        .then([&](int) { settled.fetch_add(1); })
                        .onError([&](const std::exception_ptr&) { settled.fetch_add(1); });
                }
            });
        }

        std::this_thread::sleep_for(std::chrono::milliseconds{2});
        wsServer->close();
        wsServer.reset();

        for (auto& producer : producers) {
            producer.join();
        }

        int const totalCalls = producerThreads * callsPerThread;
        spinUntil([&] { return settled.load() == totalCalls; }, 300);
        REQUIRE(settled.load() == totalCalls);

        // The server must be able to drain: a stranded ticket also pins
        // `_inFlightExecutes` above zero for good, so this is the same
        // regression seen from the graceful-shutdown side.
        REQUIRE(server->drainedWithin(std::chrono::milliseconds{5000}));

        // Same port-reuse pause as the sibling disconnect tests.
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }
}

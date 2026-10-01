// SPDX-License-Identifier: Apache-2.0

#include <poll.h>
#include <sys/socket.h>

#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/io_loop.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <morph/core/timeout_scheduler.hpp>
#include <morph/core/wire.hpp>
#include <morph/journal/action_log.hpp>
#include <morph/net/detail/tcp_socket.hpp>
#include <morph/net/detail/ws_frame.hpp>
#include <morph/net/detail/ws_handshake.hpp>
#include <morph/net/socket_backend.hpp>
#include <morph/net/socket_server.hpp>
#include <morph/session/session.hpp>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "../bind_support.hpp"
#include "../owner_probe_recorder.hpp"
#include "../test_support.hpp"

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

// The same wait, pumping a bridge's owner meanwhile: a `Bridge` delivers its
// bind replies and its handlers' continuations there.
void spinUntil(morph::exec::MainThreadExecutor& owner, const std::function<bool()>& done, int maxIterations = 200) {
    for (int i = 0; i < maxIterations && !done(); ++i) {
        owner.runFor(std::chrono::milliseconds{10});
    }
}

// Polls until `backend` reports disconnected, or `maxIterations` * spinUntil's
// own 10ms step elapses (200 -> ~2s; 25 -> ~250ms for the tighter
// abortive-close races below, which already run inside their own 40-iteration
// outer loop). waitForConnected()'s own wait_for predicate is already
// satisfied while _connected is still true, so polling it with a zero timeout
// alone would spin through every iteration in a few microseconds, never
// giving the I/O loop a chance to notice the disconnect -- spinUntil's real
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
    // Reads frames sent by the client under test (a real SocketBackend),
    // which RFC 6455 §5.1 requires to mask every frame it sends.
    FakeWsServer() : _listener(morph::net::detail::TcpSocket::listen(0)), _reader(/*expectMasked=*/true) {
        // Non-blocking so `acceptAndHandshake()` can put a bound on the wait;
        // see the comment there. `tryAccept()` still hands back a *blocking*
        // connection socket, so nothing downstream changes.
        if (!_listener.setNonBlocking()) {
            throw std::runtime_error("FakeWsServer: could not put the listener into non-blocking mode");
        }
    }

    [[nodiscard]] std::uint16_t port() const { return _listener.boundPort(); }

    // Accepts the pending connection and completes a real WS handshake.
    //
    // The client (a SocketBackend under test) is already connecting
    // concurrently on its I/O loop by the time this is called, so this
    // normally returns promptly -- but "normally" is not a bound. A blocking
    // `accept()` here parks the *main test thread* with nothing else in the
    // process able to satisfy it if the client never connects, which is how a
    // net test hangs indefinitely in `accept()` under
    // concurrent machine load: a hang costs a whole CI job, where a failure
    // costs one line. Every test in this file goes through here, so bounding
    // it once bounds all of them.
    //
    // This is a bound on a *starvation* failure, not a fix for whatever
    // caused the client not to connect: if this throws, the thing to
    // investigate is the client, not the timeout.
    void acceptAndHandshake() {
        _socket = acceptWithin(std::chrono::seconds{20});
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
        linger l{};
        l.l_onoff = 1;
        l.l_linger = 0;
        ::setsockopt(_socket.nativeHandle(), SOL_SOCKET, SO_LINGER, &l, sizeof(l));
        _socket = morph::net::detail::TcpSocket{};
    }

    // Shrinks the receive buffer to make a subsequent large write from the
    // peer fill the kernel's TCP window quickly. Combined with never calling
    // recv() again, this reliably blocks the peer's `send()` -- for tests
    // exercising `SO_SNDTIMEO` without needing a multi-megabyte
    // payload or a multi-second wait.
    void stopReadingWithTinyReceiveBuffer() {
        int const tinyBuf = 2048;
        ::setsockopt(_socket.nativeHandle(), SOL_SOCKET, SO_RCVBUF, &tinyBuf, sizeof(tinyBuf));
    }

private:
    // `poll()` supplies the bound and `tryAccept()` never parks, so the wait
    // is bounded even in the case the listener's own docs call out: a peer
    // that resets between the readability report and the `accept` takes the
    // pending connection away again, and a blocking `accept()` would park on
    // the wakeup it had already consumed.
    morph::net::detail::TcpSocket acceptWithin(std::chrono::milliseconds timeout) {
        auto const deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            if (auto accepted = _listener.tryAccept()) {
                return std::move(*accepted);
            }
            auto const waited = morph::net::detail::pollUntil(_listener.nativeHandle(), POLLIN, deadline);
            if (waited.outcome == morph::net::detail::PollOutcome::kTimedOut) {
                throw std::runtime_error("FakeWsServer::acceptAndHandshake: no client connected within " +
                                         std::to_string(timeout.count()) + "ms");
            }
            if (waited.outcome == morph::net::detail::PollOutcome::kFailed) {
                throw std::runtime_error("FakeWsServer::acceptAndHandshake: poll() failed: " +
                                         std::system_category().message(waited.error));
            }
        }
    }

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
    morph::exec::MainThreadExecutor bridgeOwner;
    morph::bridge::Bridge bridge{std::move(backendPtr), bridgeOwner};
    morph::bridge::BridgeHandler<SbEchoModel> handler{bridge, &bridgeOwner};

    std::atomic<int> result{-1};
    handler.execute(SbEchoAction{99}).then([&](int val) { result.store(val); }).onError([](const std::exception_ptr&) {
    });

    spinUntil(bridgeOwner, [&] { return result.load() != -1; });
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
    morph::exec::MainThreadExecutor bridgeOwner;
    morph::bridge::Bridge bridge{std::move(backendPtr), bridgeOwner};
    morph::bridge::BridgeHandler<SbEchoModel> handler{bridge, &bridgeOwner};

    std::atomic<bool> errorFired{false};
    handler.execute(SbEchoFail{}).then([](int) {}).onError([&](const std::exception_ptr& exc) {
        try {
            std::rethrow_exception(exc);
        } catch (const std::runtime_error&) {
            errorFired.store(true);
        }
    });

    spinUntil(bridgeOwner, [&] { return errorFired.load(); });
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
    morph::exec::MainThreadExecutor bridgeOwner;
    morph::bridge::Bridge bridge{std::move(backendPtr), bridgeOwner};
    morph::bridge::BridgeHandler<SbEchoModel> handler{bridge, &bridgeOwner};

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
    spinUntil(bridgeOwner, [&] { return resolved.load() == numCalls; }, 500);
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
    morph::exec::MainThreadExecutor bridgeOwner;
    morph::bridge::Bridge bridgeA{std::move(backendA), bridgeOwner};
    morph::bridge::Bridge bridgeB{std::move(backendB), bridgeOwner};
    morph::bridge::BridgeHandler<SbEchoModel> handlerA{bridgeA, &bridgeOwner};
    morph::bridge::BridgeHandler<SbEchoModel> handlerB{bridgeB, &bridgeOwner};

    std::atomic<int> lastA{-1};
    std::atomic<int> lastB{-1};
    handlerA.execute(SbEchoAction{11}).then([&](int v) { lastA.store(v); }).onError([](const std::exception_ptr&) {});
    handlerB.execute(SbEchoAction{22}).then([&](int v) { lastB.store(v); }).onError([](const std::exception_ptr&) {});

    spinUntil(bridgeOwner, [&] { return lastA.load() != -1 && lastB.load() != -1; });
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
    morph::exec::MainThreadExecutor bridgeOwner;
    morph::bridge::Bridge bridgeA{std::move(backendA), bridgeOwner};
    morph::bridge::Bridge bridgeB{std::move(backendB), bridgeOwner};
    morph::bridge::BridgeHandler<SbCounterModel, morph::bridge::AllowShared> fromA{bridgeA, &bridgeOwner};
    morph::bridge::BridgeHandler<SbCounterModel, morph::bridge::AllowShared> fromB{bridgeB, &bridgeOwner};

    // Two genuinely separate clients, two sockets, one server-side directory.
    std::atomic<int> lastA{-1};
    fromA.execute(SbBump{.id = 77, .by = 10})
        .then([&](const SbTotal& res) { lastA.store(res.value); })
        .onError([](const std::exception_ptr&) {});
    spinUntil(bridgeOwner, [&] { return lastA.load() != -1; });
    REQUIRE(lastA.load() == 10);

    std::atomic<int> lastB{-1};
    fromB.execute(SbBump{.id = 77, .by = 5})
        .then([&](const SbTotal& res) { lastB.store(res.value); })
        .onError([](const std::exception_ptr&) {});
    spinUntil(bridgeOwner, [&] { return lastB.load() != -1; });
    // 15, not 5: the second client attached to the first client's instance.
    REQUIRE(lastB.load() == 15);

    std::atomic<int> keyCount{-1};
    fromB.instances()
        .then([&](const std::vector<std::int64_t>& keys) { keyCount.store(static_cast<int>(keys.size())); })
        .onError([](const std::exception_ptr&) {});
    spinUntil(bridgeOwner, [&] { return keyCount.load() != -1; });
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
    morph::exec::MainThreadExecutor bridgeOwner;
    morph::bridge::Bridge sharedBridge{std::move(shared), bridgeOwner};
    morph::bridge::Bridge privBridge{std::move(priv), bridgeOwner};
    morph::bridge::BridgeHandler<SbCounterModel, morph::bridge::AllowShared> joined{sharedBridge, &bridgeOwner};
    morph::bridge::BridgeHandler<SbCounterModel> alone{privBridge, &bridgeOwner};

    std::atomic<int> lastShared{-1};
    joined.execute(SbBump{.id = 88, .by = 30})
        .then([&](const SbTotal& res) { lastShared.store(res.value); })
        .onError([](const std::exception_ptr&) {});
    spinUntil(bridgeOwner, [&] { return lastShared.load() != -1; });
    REQUIRE(lastShared.load() == 30);

    std::atomic<int> lastPriv{-1};
    alone.execute(SbBump{.id = 88, .by = 1})
        .then([&](const SbTotal& res) { lastPriv.store(res.value); })
        .onError([](const std::exception_ptr&) {});
    spinUntil(bridgeOwner, [&] { return lastPriv.load() != -1; });
    // Opted out, so it registered its own instance and counts from zero.
    REQUIRE(lastPriv.load() == 1);
}

TEST_CASE("SocketBackend: a fire-and-forget deregister's reply is not taken for a bind's", "[net][socket_backend]") {
    // `deregisterModel` sends fire-and-forget, but the server still answers
    // it with an `ok` carrying the request's `callId`. A bind that follows it
    // must match its own reply, not the deregister's.
    //
    // A private re-point is the shortest path to the collision: with an
    // empty primary it deregisters `current` and then immediately registers a
    // fresh instance over the same connection, with the deregister's reply in
    // flight ahead of the register's own.
    //
    // The assertion has to be that the returned id is a *real, different* id:
    // "the bind did not throw" holds with the bug present, and so does "an id
    // came back" if 0 is allowed to count as one.
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
    auto const mid2 = morph::testing::bindAttach(backend, "SbEchoModel", nullptr, "", mid1);
    REQUIRE(mid2.v != 0U);
    REQUIRE(mid2.v != mid1.v);

    // …and the id handed back has to be one the server actually holds, which
    // is what proves the reply that woke the register was the register's own
    // rather than some other message's that happened to carry an id.
    // Hand-built ActionCall (rather than a BridgeHandler) so the backend's
    // own bind result is the thing under test; the raw reply body is
    // kept as a string so this test needs no JSON dependency of its own.
    morph::backend::detail::ActionCall call{
        .modelTypeId = "SbEchoModel",
        .actionTypeId = "SbEchoAction",
        .serializeAction = [](const void*) { return std::string{R"({"value":7})"}; },
        .deserializeResult =
            [](std::string_view body) { return std::static_pointer_cast<void>(std::make_shared<std::string>(body)); },
        .localOp = nullptr,
        .session = {},
    };

    morph::exec::MainThreadExecutor cbOwner;
    std::atomic<bool> settled{false};
    std::string echoed;
    backend.execute(mid2, std::move(call), &cbOwner)
        .then([&](const std::shared_ptr<void>& res) {
            echoed = *std::static_pointer_cast<std::string>(res);
            settled.store(true);
        })
        .onError([&](const std::exception_ptr&) { settled.store(true); });
    spinUntil(cbOwner, [&] { return settled.load(); });
    REQUIRE(echoed == "7");
}

TEST_CASE("SocketBackend: registerModel on a never-connected socket throws, does not hang",
          "[net][socket_backend][disconnect]") {
    // Port 1 is reserved (root-only) on Linux/macOS and never listening — the
    // socket never connects, so the I/O loop exits without retrying.
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

TEST_CASE("SocketBackend: a shared bind with an empty primary degrades to a private register",
          "[net][socket_backend]") {
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    morph::net::SocketServer wsServer{*server, 0};
    REQUIRE(wsServer.listen());

    std::string const url = "ws://127.0.0.1:" + std::to_string(static_cast<unsigned>(wsServer.port()));
    morph::net::SocketBackend backend{url};
    REQUIRE(backend.waitForConnected());

    auto mid = morph::testing::bindShared(backend, "SbEchoModel", nullptr, "");
    REQUIRE(mid.v != 0U);
}

TEST_CASE("SocketBackend: a shared bind with a primary reaches the server's register-or-attach directory",
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

    auto midA = morph::testing::bindShared(backendA, "SbCounterModel", nullptr, "sb-shared-key-1");
    REQUIRE(midA.v != 0U);
    // The second caller naming the same primary reaches the same instance
    // rather than creating a new one -- confirms this actually went through
    // the server's shared directory, not just that some instance came back.
    auto midB = morph::testing::bindShared(backendB, "SbCounterModel", nullptr, "sb-shared-key-1");
    REQUIRE(midB.v == midA.v);
}

TEST_CASE("SocketBackend: a re-point with an empty primary and current==0 registers a private instance",
          "[net][socket_backend]") {
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    morph::net::SocketServer wsServer{*server, 0};
    REQUIRE(wsServer.listen());

    std::string const url = "ws://127.0.0.1:" + std::to_string(static_cast<unsigned>(wsServer.port()));
    morph::net::SocketBackend backend{url};
    REQUIRE(backend.waitForConnected());

    // current == 0: no prior instance to give up, so this is just a private
    // register.
    auto mid = morph::testing::bindAttach(backend, "SbEchoModel", nullptr, "", morph::exec::detail::ModelId{0});
    REQUIRE(mid.v != 0U);
}

TEST_CASE("SocketBackend: a re-point to an empty primary deregisters the instance being given up",
          "[net][socket_backend]") {
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    morph::net::SocketServer wsServer{*server, 0};
    REQUIRE(wsServer.listen());

    std::string const url = "ws://127.0.0.1:" + std::to_string(static_cast<unsigned>(wsServer.port()));
    morph::net::SocketBackend backend{url};
    REQUIRE(backend.waitForConnected());

    // A *shared* instance, so its release is externally observable via
    // listInstances -- unlike a private one, which never enters the directory.
    auto shared = morph::testing::bindShared(backend, "SbCounterModel", nullptr, "handoff-key");
    REQUIRE(shared.v != 0U);

    // current != 0, empty primary: give up the shared instance for a fresh
    // private one.
    REQUIRE_NOTHROW(morph::testing::bindAttach(backend, "SbCounterModel", nullptr, "", shared));

    // The instance held under "handoff-key" must be released.
    bool released = false;
    for (int i = 0; i < 100 && !released; ++i) {
        auto keys = backend.listInstances("SbCounterModel");
        released = std::ranges::find(keys, "handoff-key") == keys.end();
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

    auto midB = morph::testing::bindAttach(backendB, "SbCounterModel", nullptr, "sb-assigned-key-1",
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

TEST_CASE("SocketBackend: a shared bind surfaces the server's error reply when registration is unauthorized",
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
        morph::testing::bindAttach(backend, "SbEchoModel", nullptr, "k1", morph::exec::detail::ModelId{0}),
        Catch::Matchers::ContainsSubstring("register failed"));
}

TEST_CASE("SocketBackend: a bind on a disconnected socket rejects instead of hanging",
          "[net][socket_backend][disconnect]") {
    morph::net::SocketBackend backend{"ws://127.0.0.1:1"};
    REQUIRE_FALSE(backend.waitForConnected(std::chrono::milliseconds{200}));
    REQUIRE_THROWS_AS(
        morph::testing::bindAttach(backend, "SbEchoModel", nullptr, "k1", morph::exec::detail::ModelId{0}),
        morph::backend::DisconnectedError);
}

TEST_CASE("SocketBackend: ~SocketBackend does not hang against a peer that stalls the handshake",
          "[net][socket_backend]") {
    // A TCP listener need never call
    // accept() for a connecting client's connect() to succeed -- the kernel
    // completes the three-way handshake into the listen backlog on its own.
    // That gives a peer that is connected at the TCP level but writes
    // nothing, which used to leave the client's WS handshake read (no
    // SO_RCVTIMEO of its own) blocked forever, and the fd was not yet
    // published to `_socket` for the destructor's escape hatch to reach.
    morph::net::detail::TcpSocket const listener = morph::net::detail::TcpSocket::listen(0);
    std::uint16_t const port = listener.boundPort();

    morph::net::SocketBackend::Config cfg;
    cfg.reconnectEnabled = false;
    cfg.handshakeTimeout = std::chrono::milliseconds{300};

    auto backend = std::make_unique<morph::net::SocketBackend>(
        "ws://127.0.0.1:" + std::to_string(static_cast<unsigned>(port)), cfg);
    std::this_thread::sleep_for(std::chrono::milliseconds{100});  // let the I/O loop's connect() land

    // Both the backend and the promise are *owned* by the thread rather than
    // captured by reference. The backend, because if the destructor never
    // returns and this thread is detached below, the TEST_CASE's own `backend`
    // must not still hold (and then destroy, at scope exit) the same object
    // the detached thread is destroying -- that would be a second, concurrent
    // destructor call on it. The promise, because on that same path the
    // REQUIRE below throws and unwinds this scope, so a promise living here
    // would be destroyed underneath the detached thread's `set_value()`.
    auto destroyed = std::make_shared<std::promise<void>>();
    std::future<void> const destroyedFuture = destroyed->get_future();
    std::thread destroyer([owned = std::move(backend), destroyed]() mutable {
        owned.reset();
        destroyed->set_value();
    });
    bool const finished = destroyedFuture.wait_for(std::chrono::seconds{5}) == std::future_status::ready;
    if (finished) {
        destroyer.join();
    } else {
        destroyer.detach();  // still stuck in the hung destructor; leaking beats hanging the whole suite
    }
    REQUIRE(finished);
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

TEST_CASE("SocketBackend: two synchronous calls in flight at once both get their own reply", "[net][socket_backend]") {
    // Each synchronous verb is the posted request a bind makes, waited for on
    // its caller's thread and matched by callId, so two callers never share
    // one reply slot and neither is refused.
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto authz = std::make_shared<SlowAuthorizer>(std::chrono::milliseconds{150});
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool, authz);
    morph::net::SocketServer wsServer{*server, 0};
    REQUIRE(wsServer.listen());

    std::string const url = "ws://127.0.0.1:" + std::to_string(static_cast<unsigned>(wsServer.port()));
    morph::net::SocketBackend backend{url};
    REQUIRE(backend.waitForConnected());

    std::atomic<int> failed{0};
    std::vector<std::uint64_t> ids(2, 0);
    auto attempt = [&](std::size_t slot) {
        try {
            ids[slot] = backend.registerModel("SbEchoModel", nullptr).v;
        } catch (const std::exception&) {
            failed.fetch_add(1);
        }
    };
    std::thread t1{attempt, 0};
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    std::thread t2{attempt, 1};
    t1.join();
    t2.join();

    REQUIRE(failed.load() == 0);
    REQUIRE(ids[0] != 0);
    REQUIRE(ids[1] != 0);
    REQUIRE(ids[0] != ids[1]);
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
    // this point -- the client is genuinely waiting for the reply, not merely
    // about to ask.
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    wsServer->close();
    wsServer.reset();
    caller.join();

    REQUIRE(threw);
    REQUIRE(what.contains("register failed"));
    REQUIRE(what.contains("disconnected"));
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
    call.serializeAction = [](const void*) { return std::string{"{}"}; };
    call.deserializeResult = [](std::string_view) -> std::shared_ptr<void> { return nullptr; };

    // Drain the outgoing "execute" request off the wire first, so the pending
    // entry genuinely exists server-side before the garbage reply lands.
    std::thread serverThread{[&] { (void)fake.receiveEnvelope(); }};
    morph::exec::MainThreadExecutor cbOwner;
    auto comp = backendPtr->execute(morph::exec::detail::ModelId{1}, std::move(call), &cbOwner);
    serverThread.join();

    std::atomic<bool> gotProtocolError{false};
    comp.onError([&](const std::exception_ptr& exc) {
        try {
            std::rethrow_exception(exc);
        } catch (const std::exception& e) {
            if (std::string{e.what()}.contains("protocol error")) {
                gotProtocolError.store(true);
            }
        }
    });

    fake.sendFrame(morph::net::detail::WsOpcode::kText, "this is not a valid envelope");

    spinUntil(cbOwner, [&] { return gotProtocolError.load(); });
    REQUIRE(gotProtocolError.load());
}

TEST_CASE(
    "SocketBackend: an undecodable message from the server while a synchronous call is in flight fails that call",
    "[net][socket_backend][fault-injection]") {
    FakeWsServer fake;
    morph::net::SocketBackend backend{"ws://127.0.0.1:" + std::to_string(static_cast<unsigned>(fake.port()))};
    fake.acceptAndHandshake();
    REQUIRE(backend.waitForConnected());

    std::thread serverThread{[&] {
        (void)fake.receiveEnvelope();  // the register request
        fake.sendFrame(morph::net::detail::WsOpcode::kText, "this is not a valid envelope either");
    }};

    // The reply cannot be matched to any call, so every pending call -- the
    // register this thread waits on included -- fails with a protocol error
    // instead of hanging.
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
    call.serializeAction = [](const void*) { return std::string{"{}"}; };
    call.deserializeResult = [](std::string_view body) -> std::shared_ptr<void> {
        return std::make_shared<int>(std::stoi(std::string{body}));
    };

    std::thread serverThread{[&] {
        auto env = fake.receiveEnvelope();
        fake.sendFrame(morph::net::detail::WsOpcode::kText, morph::wire::encode(morph::wire::makeOk(env.callId, "7")));
    }};

    morph::exec::MainThreadExecutor cbOwner;
    std::atomic<int> result{-1};
    auto comp = backend.execute(morph::exec::detail::ModelId{1}, std::move(call), &cbOwner);
    comp.then([&](const std::shared_ptr<void>& v) { result.store(*std::static_pointer_cast<int>(v)); })
        .onError([](const std::exception_ptr&) {});

    spinUntil(cbOwner, [&] { return result.load() != -1; });
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
    call.serializeAction = [](const void*) { return std::string{"{}"}; };
    call.deserializeResult = [](std::string_view body) -> std::shared_ptr<void> {
        return std::make_shared<int>(std::stoi(std::string{body}));  // throws on non-numeric garbage
    };

    std::thread serverThread{[&] {
        auto env = fake.receiveEnvelope();
        fake.sendFrame(morph::net::detail::WsOpcode::kText,
                       morph::wire::encode(morph::wire::makeOk(env.callId, "not-a-number")));
    }};

    morph::exec::MainThreadExecutor cbOwner;
    std::atomic<bool> gotError{false};
    auto comp = backend.execute(morph::exec::detail::ModelId{1}, std::move(call), &cbOwner);
    comp.then([](const std::shared_ptr<void>&) {}).onError([&](const std::exception_ptr&) { gotError.store(true); });

    spinUntil(cbOwner, [&] { return gotError.load(); });
    REQUIRE(gotError.load());
    serverThread.join();
}

TEST_CASE("SocketBackend: a send blocked past sendTimeout tears the connection down instead of desyncing it",
          "[net][socket_backend][fault-injection]") {
    // `TcpSocket::sendAll` can throw
    // having already written part of a frame -- `SO_SNDTIMEO` firing
    // mid-send is exactly this, reachable whenever a peer stops reading. The
    // old `sendFrame` swallowed that exception without marking the
    // connection unusable, so a later frame would land in the middle of the
    // truncated one instead of the connection being torn down. Reproduced by
    // shrinking the peer's receive buffer and never draining it, so a large
    // enough payload reliably blocks the client's `send()` until
    // `sendTimeout` fires.
    FakeWsServer fake;
    morph::net::SocketBackend::Config cfg;
    cfg.reconnectEnabled = false;
    cfg.sendTimeout = std::chrono::milliseconds{300};
    morph::net::SocketBackend backend{"ws://127.0.0.1:" + std::to_string(static_cast<unsigned>(fake.port())), cfg};
    fake.acceptAndHandshake();
    REQUIRE(backend.waitForConnected());
    fake.stopReadingWithTinyReceiveBuffer();

    morph::backend::detail::ActionCall call;
    call.modelTypeId = "SbEchoModel";
    call.actionTypeId = "SbEchoAction";
    // The payload rides in `call.action` rather than in a capture:
    // `serializeAction` is a function pointer, so it has nowhere to capture to.
    call.action = std::make_shared<std::string>(std::size_t{4} * 1024 * 1024, 'x');  // far past the receive buffer
    call.serializeAction = [](const void* payload) { return *static_cast<const std::string*>(payload); };
    call.deserializeResult = [](std::string_view) -> std::shared_ptr<void> { return nullptr; };
    (void)backend.execute(morph::exec::detail::ModelId{1}, std::move(call), nullptr);

    REQUIRE(waitForDisconnect(backend));
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

TEST_CASE("SocketBackend: a Ping frame followed by an abortive close does not hang the loop",
          "[net][socket_backend][disconnect]") {
    // Aims at drainFrames()'s own Ping-echo `sendFrame(kPong, ...)` catch
    // (distinct from -- and narrower than -- the general sendFrame-races-a-
    // disconnect family closed above): here the *same* I/O loop reads the
    // Ping and then, moments later, tries to write the Pong back on a socket
    // an abortive RST may have already torn down. The RST and the read+echo
    // sequence race each other with no synchronization, so whether this
    // specific catch fires depends on exactly how fast the RST lands versus
    // how fast the I/O loop turns the Ping around -- not reliably won on
    // every run. What *is* guaranteed regardless of who wins: the I/O loop
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

TEST_CASE("SocketBackend: a Close frame followed by an abortive close does not hang the loop",
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
    // with reconnectEnabled=false, the I/O loop already returned for good.
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

    morph::exec::MainThreadExecutor cbOwner;
    morph::backend::detail::ActionCall call;
    call.modelTypeId = "SbEchoModel";
    call.actionTypeId = "SbEchoAction";
    call.serializeAction = [](const void*) { return std::string{"{}"}; };
    call.deserializeResult = [](std::string_view) -> std::shared_ptr<void> { return nullptr; };

    std::atomic<bool> gotDisconnected{false};
    auto comp = backend.execute(morph::exec::detail::ModelId{1}, std::move(call), &cbOwner);
    comp.onError([&](const std::exception_ptr& exc) {
        try {
            std::rethrow_exception(exc);
        } catch (const morph::backend::DisconnectedError&) {
            gotDisconnected.store(true);
        }
    });
    spinUntil(cbOwner, [&] { return gotDisconnected.load(); });
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
    morph::exec::MainThreadExecutor bridgeOwner;
    morph::bridge::Bridge bridge{std::move(backendPtr), bridgeOwner};
    morph::bridge::BridgeHandler<SbSlowModel> handler{bridge, &bridgeOwner};

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

    spinUntil(bridgeOwner, [&] { return gotDisconnected.load(); }, 200);
    REQUIRE(gotDisconnected.load());
}

TEST_CASE("SocketBackend: execute() racing a disconnect never leaves a Completion unresolved",
          "[net][socket_backend][disconnect]") {
    // Targets the window between execute()'s caller-side `connected` check
    // and the I/O loop filing the call: execute() reads `connected` on the
    // calling thread and posts to the loop, and a disconnect handled on the
    // loop in between has already swept the pending table the call is about
    // to join. The loop re-checks `connected` when it files the call and
    // rejects it with DisconnectedError instead; without that re-check the
    // Completion would stay unresolved -- a silent hang, not a crash.
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
    // That shape belongs to "many concurrent executes racing a disconnect
    // leave the server able to drain" below -- a connection with several
    // executes in flight dropping -- whose own hang would masquerade as a
    // failure of *this* fix; the two are kept separate so a regression in
    // either one fails where it is diagnosed.
    for (int iter = 0; iter < 3; ++iter) {
        morph::exec::ThreadPoolExecutor serverPool{2};
        auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
        auto wsServer = std::make_unique<morph::net::SocketServer>(*server, 0);
        REQUIRE(wsServer->listen());

        std::string const url = "ws://127.0.0.1:" + std::to_string(static_cast<unsigned>(wsServer->port()));
        auto backendPtr = std::make_unique<morph::net::SocketBackend>(url);
        REQUIRE(backendPtr->waitForConnected());
        morph::exec::MainThreadExecutor bridgeOwner;
        morph::bridge::Bridge bridge{std::move(backendPtr), bridgeOwner};
        morph::bridge::BridgeHandler<SbEchoModel> handler{bridge, &bridgeOwner};

        constexpr int callsPerThread = 3;
        constexpr int producerThreads = 2;
        std::atomic<int> settled{0};
        std::vector<std::thread> producers;
        producers.reserve(producerThreads);
        for (int t = 0; t < producerThreads; ++t) {
            producers.emplace_back([&] {
                for (int i = 0; i < callsPerThread; ++i) {
                    // A handler is called on its bridge's owner: a producer
                    // thread posts the call there.
                    bridgeOwner.post([&handler, &settled, i] {
                        handler.execute(SbEchoAction{i})
                            .then([&settled](int) { settled.fetch_add(1); })
                            .onError([&settled](const std::exception_ptr&) { settled.fetch_add(1); });
                    });
                }
            });
        }

        // Drop the connection while calls are still being issued and in
        // flight, aiming squarely at the window between "still connected"
        // and "the entry is in _pending".
        // The owner runs the calls posted so far while the connection drops.
        bridgeOwner.runFor(std::chrono::milliseconds{2});
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
        spinUntil(bridgeOwner, [&] { return settled.load() == totalCalls; }, 200);
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
    // Targets the windows between a caller-side "am I connected" check
    // (deregisterModel()'s `connected` load, execute()'s) and the write that
    // follows on the I/O loop: the caller's check passes, the loop handles
    // the disconnect, and only then runs the posted write. Every write runs
    // on the loop and checks the connection there, so it finds it gone rather
    // than writing to a closed one; registerModel() meanwhile waits for a
    // reply the disconnect rejects. Same statistical technique as
    // "execute() racing a disconnect never leaves a Completion unresolved"
    // above, across every caller that ends in a write, since it is the same
    // window whichever caller hits it.
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
        hammer.reserve(3);
        for (int t = 0; t < 3; ++t) {
            hammer.emplace_back([&] {
                while (!stop.load(std::memory_order_relaxed)) {
                    backend.deregisterModel(mid);
                    morph::backend::detail::ActionCall call;
                    call.modelTypeId = "SbEchoModel";
                    call.actionTypeId = "SbEchoAction";
                    call.serializeAction = [](const void*) { return std::string{"{}"}; };
                    call.deserializeResult = [](std::string_view) -> std::shared_ptr<void> { return nullptr; };
                    (void)backend.execute(mid, std::move(call), nullptr);
                    try {
                        (void)backend.registerModel("SbEchoModel", nullptr);
                    } catch (const std::exception&) {
                        // "disconnected" or success are both fine here --
                        // only a hang or crash would be a failure.
                        continue;
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
    // wire::kExecuteTimeoutMessage branch: a server-side LimitPolicy::executeTimeout
    // reply must resolve the Completion with backend::TimeoutError specifically,
    // the same type QtWebSocketBackend/SimulatedRemoteBackend give callers for
    // this case -- not the generic std::runtime_error the `else` branch below it
    // produces for an arbitrary `err` message. Mirrors
    // tests/test_limit_policy.cpp's SimulatedRemoteBackend analog of this same
    // scenario, over the real WebSocket transport instead.
    morph::exec::ThreadPoolExecutor serverPool{4};
    morph::backend::LimitPolicy policy;
    policy.executeTimeout = std::chrono::milliseconds{50};
    morph::backend::ServerConfig serverConfig;
    serverConfig.limits = policy;
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool, serverConfig);
    morph::net::SocketServer wsServer{*server, 0};
    REQUIRE(wsServer.listen());

    std::string const url = "ws://127.0.0.1:" + std::to_string(static_cast<unsigned>(wsServer.port()));
    auto backendPtr = std::make_unique<morph::net::SocketBackend>(url);
    REQUIRE(backendPtr->waitForConnected());
    morph::exec::MainThreadExecutor bridgeOwner;
    morph::bridge::Bridge bridge{std::move(backendPtr), bridgeOwner};
    // SbSlowModel's execute() sleeps 300ms unconditionally -- well past the
    // 50ms executeTimeout above, so the server's timeout scheduler fires
    // before the strand result ever comes back.
    morph::bridge::BridgeHandler<SbSlowModel> handler{bridge, &bridgeOwner};

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

    spinUntil(bridgeOwner, [&] { return gotTimeoutError.load() || gotSomethingElse.load(); });
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
    // repeatedly is correct: it returns true as soon as the I/O loop's
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
    // Where the reconnect handler is posted: a thread of its own, never the
    // backend's I/O loop.
    morph::exec::ThreadPoolExecutor handlerExec{1};
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
            backend->setReconnectHandler(onReconnect, &handlerExec);
        }  // first server destroyed -> the backend observes the drop and retries

        std::this_thread::sleep_for(std::chrono::milliseconds{100});
        restarted = std::make_unique<morph::net::SocketServer>(*server, port);
        if (!restarted->listen()) {
            throw std::runtime_error("ReconnectFixture: re-listen failed");
        }
    }
};

}  // namespace

TEST_CASE("SocketBackend: the reconnect handler is posted to the executor it was installed with",
          "[net][socket_backend][disconnect][owner]") {
    // The handler never runs on the I/O loop: after a reconnect the loop posts
    // it to its executor and goes on, so a handler that re-registers --
    // waiting for a reply only the loop can deliver -- cannot wait on itself.
    ReconnectFixture fixture;
    std::atomic<bool> onItsExecutor{false};
    std::atomic<bool> handlerSucceeded{false};

    fixture.bounce([&] {
        onItsExecutor.store(morph::exec::runningOn(fixture.handlerExec));
        handlerSucceeded.store(fixture.backend->registerModel("SbEchoModel", nullptr).v != 0U);
    });

    spinUntil([&] { return handlerSucceeded.load(); }, 500);
    CHECK(onItsExecutor.load());
    CHECK(handlerSucceeded.load());
    CHECK(fixture.backend->registerModel("SbEchoModel", nullptr).v != 0U);
}

TEST_CASE("SocketBackend: a throwing reconnect handler leaves the transport usable",
          "[net][socket_backend][disconnect]") {
    // An exception escaping the handler is its executor's to log; the
    // transport goes on.
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
    // Same shape as the std::exception case above, with a non-std::exception
    // value.
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
TEST_CASE("SocketBackend: many concurrent executes racing a disconnect leave the server able to drain",
          "[net][socket_backend][disconnect]") {
    // The real disconnect teardown at the transport level: dropping the
    // connection reclaims that connection's models, so the executes still in
    // flight for one model split into some that find the model and some that
    // reject with "model not found". Every one must be answered and the
    // server must drain; per-model order across such a split is pinned
    // deterministically by tests/test_remote_execute_ordering.cpp.
    //
    // The visible failure is not this test's assertions: it is the teardown
    // below it. An execute that is never answered leaves the in-flight count
    // above zero, and a pool worker that never returns hangs
    // `~ThreadPoolExecutor` in join() -- turned into a failure by ctest's
    // per-test TIMEOUT.
    //
    // A *probabilistic* shape, kept because it is the only test that drives
    // the real disconnect teardown, and because it costs ~0.2s. It is
    // deliberately the "heavier stress shape" the sibling "execute() racing a
    // disconnect never leaves a Completion unresolved" case above avoids.
    for (int iter = 0; iter < 3; ++iter) {
        morph::exec::ThreadPoolExecutor serverPool{4};
        auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
        auto wsServer = std::make_unique<morph::net::SocketServer>(*server, 0);
        REQUIRE(wsServer->listen());

        std::string const url = "ws://127.0.0.1:" + std::to_string(static_cast<unsigned>(wsServer->port()));
        auto backendPtr = std::make_unique<morph::net::SocketBackend>(url);
        REQUIRE(backendPtr->waitForConnected());
        morph::exec::MainThreadExecutor bridgeOwner;
        morph::bridge::Bridge bridge{std::move(backendPtr), bridgeOwner};
        // One shared handler, so every call targets the *same* model id and
        // therefore the same execute-ordering gate -- the gate is per-model,
        // so calls spread across instances would not queue behind each other
        // at all.
        morph::bridge::BridgeHandler<SbEchoModel> handler{bridge, &bridgeOwner};

        constexpr int callsPerThread = 60;
        constexpr int producerThreads = 8;
        std::atomic<int> settled{0};
        std::vector<std::thread> producers;
        producers.reserve(producerThreads);
        for (int producer = 0; producer < producerThreads; ++producer) {
            producers.emplace_back([&] {
                for (int i = 0; i < callsPerThread; ++i) {
                    // A handler is called on its bridge's owner: a producer
                    // thread posts the call there.
                    bridgeOwner.post([&handler, &settled, i] {
                        handler.execute(SbEchoAction{i})
                            .then([&settled](int) { settled.fetch_add(1); })
                            .onError([&settled](const std::exception_ptr&) { settled.fetch_add(1); });
                    });
                }
            });
        }

        // The owner runs the calls posted so far while the connection drops.
        bridgeOwner.runFor(std::chrono::milliseconds{2});
        wsServer->close();
        wsServer.reset();

        for (auto& producer : producers) {
            producer.join();
        }

        int const totalCalls = producerThreads * callsPerThread;
        spinUntil(bridgeOwner, [&] { return settled.load() == totalCalls; }, 300);
        REQUIRE(settled.load() == totalCalls);

        // The server must be able to drain: an execute never answered pins
        // the in-flight count above zero for good.
        REQUIRE(morph::testing::awaitAnswer(
            [&](auto& owner) { return server->drainedWithin(std::chrono::milliseconds{5000}, owner); }));

        // Same port-reuse pause as the sibling disconnect tests.
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }
}

// ── The structural registration surface ─────────────────────────────────────
//
// `SocketBackend` overrides `bindModel`/`promoteModel` natively: a control call
// goes out on the same callId-multiplexed path `execute` uses and is settled by
// the I/O loop's read loop, and the synchronous verbs wait on that same path.
// The tests below pin that property, not just the functional result.

namespace {

// Drains `exec` on the calling thread until `done`, or gives up. The caller's
// executor is a MainThreadExecutor precisely so that "the continuation ran"
// and "the caller pumped it" are separable events.
bool drainUntil(morph::exec::MainThreadExecutor& exec, const std::atomic<bool>& done, int maxIterations = 500) {
    for (int i = 0; i < maxIterations && !done.load(); ++i) {
        if (!exec.runOnce()) {
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
    }
    return done.load();
}

morph::backend::detail::BindRequest privateBind(std::string typeId) {
    return morph::backend::detail::BindRequest{
        .typeId = std::move(typeId), .factory = nullptr, .contextKey = {}, .primary = {}, .current = {}};
}

}  // namespace

TEST_CASE(
    "SocketBackend: bindModel reaches the server for each BindRequest shape and settles on the caller's executor",
    "[net][socket_backend][registration-surface]") {
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    morph::net::SocketServer wsServer{*server, 0};
    REQUIRE(wsServer.listen());

    // Declared before `backend`, so it is destroyed *after* it: `~SocketBackend`
    // joins the I/O loop, which can call `post()` on this executor right up
    // until that join completes. With the reverse order, TSan caught the I/O
    // thread still running -- and still able to call `post()` -- while this
    // executor's own destructor was tearing down its condition variable on the
    // main thread -- a data race in pthread_cond_destroy.
    morph::exec::MainThreadExecutor callerExec;
    morph::net::SocketBackend backend{"ws://127.0.0.1:" + std::to_string(static_cast<unsigned>(wsServer.port()))};
    REQUIRE(backend.waitForConnected());

    morph::exec::detail::ModelId bound{};
    std::atomic<bool> done{false};
    auto observe = [&](morph::exec::detail::ModelId mid) {
        bound = mid;
        done.store(true);
    };

    SECTION("empty primary, no current instance -> a private instance") {
        backend.bindModel(privateBind("SbEchoModel"), callerExec).thenDetached(observe);
        REQUIRE(drainUntil(callerExec, done));
        CHECK(bound.v != 0U);
    }

    SECTION("non-empty primary -> the server's register-or-attach directory") {
        auto first = morph::exec::detail::ModelId{};
        std::atomic<bool> firstDone{false};
        backend
            .bindModel(morph::backend::detail::BindRequest{.typeId = "SbCounterModel",
                                                           .factory = nullptr,
                                                           .contextKey = {},
                                                           .primary = "shared-key",
                                                           .current = {}},
                       callerExec)
            .thenDetached([&](morph::exec::detail::ModelId mid) {
                first = mid;
                firstDone.store(true);
            });
        REQUIRE(drainUntil(callerExec, firstDone));
        REQUIRE(first.v != 0U);

        // A second bind on the same key reaches the same live instance.
        backend
            .bindModel(morph::backend::detail::BindRequest{.typeId = "SbCounterModel",
                                                           .factory = nullptr,
                                                           .contextKey = {},
                                                           .primary = "shared-key",
                                                           .current = {}},
                       callerExec)
            .thenDetached(observe);
        REQUIRE(drainUntil(callerExec, done));
        CHECK(bound == first);
    }

    SECTION("non-empty primary plus a current instance -> a re-point") {
        auto current = backend.registerModel("SbCounterModel", nullptr);
        REQUIRE(current.v != 0U);
        backend
            .bindModel(morph::backend::detail::BindRequest{.typeId = "SbCounterModel",
                                                           .factory = nullptr,
                                                           .contextKey = {},
                                                           .primary = "repoint-key",
                                                           .current = current},
                       callerExec)
            .thenDetached(observe);
        REQUIRE(drainUntil(callerExec, done));
        CHECK(bound.v != 0U);
    }

    SECTION("promoteModel files a live instance and echoes its id back") {
        auto mid = backend.registerModel("SbCounterModel", nullptr);
        REQUIRE(mid.v != 0U);
        backend
            .promoteModel(
                morph::backend::detail::PromoteRequest{.mid = mid, .typeId = "SbCounterModel", .primary = "promoted"},
                callerExec)
            .thenDetached(observe);
        REQUIRE(drainUntil(callerExec, done));
        CHECK(bound == mid);
        CHECK(backend.listInstances("SbCounterModel") == std::vector<std::string>{"promoted"});
    }

    SECTION("promoteModel's local guards resolve without touching the wire") {
        backend
            .promoteModel(
                morph::backend::detail::PromoteRequest{
                    .mid = morph::exec::detail::ModelId{7}, .typeId = "SbCounterModel", .primary = {}},
                callerExec)
            .thenDetached(observe);
        REQUIRE(drainUntil(callerExec, done));
        CHECK(bound == morph::exec::detail::ModelId{7});
    }
}

TEST_CASE("SocketBackend: a bindModel continuation does not run until the caller's executor is pumped",
          "[net][socket_backend][registration-surface]") {
    // The whole point of the surface: the delivery thread is the caller's
    // argument, not the backend's choice. The backend settles from its I/O
    // thread; nothing may run on the caller's side until the caller pumps.
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    morph::net::SocketServer wsServer{*server, 0};
    REQUIRE(wsServer.listen());

    // Declared before `backend` -- see the identical comment on the first
    // TEST_CASE in this file that needed it: a data race on teardown.
    morph::exec::MainThreadExecutor callerExec;
    morph::net::SocketBackend backend{"ws://127.0.0.1:" + std::to_string(static_cast<unsigned>(wsServer.port()))};
    REQUIRE(backend.waitForConnected());

    std::atomic<bool> ran{false};
    std::thread::id ranOn{};
    auto completion = backend.bindModel(privateBind("SbEchoModel"), callerExec);
    completion.thenDetached([&](morph::exec::detail::ModelId) {
        ranOn = std::this_thread::get_id();
        ran.store(true);
    });

    // Give the round trip more than enough time to complete on the I/O loop.
    std::this_thread::sleep_for(std::chrono::milliseconds{200});
    CHECK_FALSE(ran.load());

    REQUIRE(drainUntil(callerExec, ran));
    CHECK(ranOn == std::this_thread::get_id());
}

// The Catch2 assertion macros, not branching logic, push this over the
// cognitive-complexity threshold -- as in the sibling fault-injection cases.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("SocketBackend: a bind settles while a synchronous register is still waiting for its reply",
          "[net][socket_backend][registration-surface]") {
    // A synchronous verb waits on the multiplexed path too, so it holds no
    // channel a bind needs: the bind is accepted, sent with its own callId,
    // and settled while the synchronous call is still waiting.
    FakeWsServer fake;
    morph::net::SocketBackend::Config cfg;
    cfg.reconnectEnabled = false;

    // Declared before `backend` -- see the identical comment on the first
    // TEST_CASE in this file that needed it: a data race on teardown.
    morph::exec::MainThreadExecutor callerExec;
    morph::net::SocketBackend backend{"ws://127.0.0.1:" + std::to_string(static_cast<unsigned>(fake.port())), cfg};
    fake.acceptAndHandshake();
    REQUIRE(backend.waitForConnected());

    std::atomic<bool> syncReturned{false};
    std::string syncOutcome;
    std::thread syncThread{[&] {
        try {
            (void)backend.registerModel("SbEchoModel", nullptr);
            syncOutcome = "returned";
        } catch (const std::exception& exc) {
            syncOutcome = exc.what();
        }
        syncReturned.store(true);
    }};
    auto syncEnv = fake.receiveEnvelope();
    REQUIRE(syncEnv.kind == "register");
    REQUIRE(syncEnv.callId != 0U);

    morph::exec::detail::ModelId bound{};
    std::atomic<bool> done{false};
    auto completion = backend.bindModel(privateBind("SbEchoModel"), callerExec);
    completion.thenDetached([&](morph::exec::detail::ModelId mid) {
        bound = mid;
        done.store(true);
    });

    auto bindEnv = fake.receiveEnvelope();
    REQUIRE(bindEnv.kind == "register");
    REQUIRE(bindEnv.callId != 0U);
    REQUIRE(bindEnv.callId != syncEnv.callId);

    // Answering only the bind settles it, with the synchronous call still waiting.
    fake.sendFrame(morph::net::detail::WsOpcode::kText,
                   morph::wire::encode(morph::wire::makeOk(bindEnv.callId, {}, 4242)));
    REQUIRE(drainUntil(callerExec, done));
    CHECK(bound == morph::exec::detail::ModelId{4242});
    CHECK_FALSE(syncReturned.load());

    fake.sendFrame(morph::net::detail::WsOpcode::kText,
                   morph::wire::encode(morph::wire::makeOk(syncEnv.callId, {}, 7)));
    syncThread.join();
    CHECK(syncOutcome == "returned");
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("SocketBackend: several binds are in flight at once and are matched by callId with replies out of order",
          "[net][socket_backend][registration-surface]") {
    // The corollary of "a bind never parks": several are outstanding at once.
    // Under the default blocking `bindModel` the first call here would never
    // return (its reply is only sent after all four have been issued), so this
    // test fails by hanging if the native override is removed.
    FakeWsServer fake;
    morph::net::SocketBackend::Config cfg;
    cfg.reconnectEnabled = false;

    // Declared before `backend` -- see the identical comment on the first
    // TEST_CASE in this file that needed it: a data race on teardown.
    morph::exec::MainThreadExecutor callerExec;
    morph::net::SocketBackend backend{"ws://127.0.0.1:" + std::to_string(static_cast<unsigned>(fake.port())), cfg};
    fake.acceptAndHandshake();
    REQUIRE(backend.waitForConnected());

    constexpr int kBinds = 4;
    std::vector<morph::async::Completion<morph::exec::detail::ModelId>> completions;
    std::vector<morph::exec::detail::ModelId> bound(kBinds);
    std::atomic<int> settled{0};
    for (int i = 0; i < kBinds; ++i) {
        completions.push_back(backend.bindModel(privateBind("SbEchoModel"), callerExec));
        completions.back().thenDetached([&, i](morph::exec::detail::ModelId mid) {
            bound[static_cast<std::size_t>(i)] = mid;
            settled.fetch_add(1);
        });
    }

    std::vector<std::uint64_t> callIds;
    for (int i = 0; i < kBinds; ++i) {
        auto env = fake.receiveEnvelope();
        REQUIRE(env.callId != 0U);
        callIds.push_back(env.callId);
    }
    REQUIRE(std::set<std::uint64_t>(callIds.begin(), callIds.end()).size() == kBinds);

    // Answered back to front: the reply router, not arrival order, decides
    // which Completion each id settles.
    for (int i = kBinds - 1; i >= 0; --i) {
        fake.sendFrame(morph::net::detail::WsOpcode::kText,
                       morph::wire::encode(morph::wire::makeOk(callIds[static_cast<std::size_t>(i)], {},
                                                               static_cast<std::uint64_t>(100 + i))));
    }

    std::atomic<bool> allDone{false};
    for (int i = 0; i < 500 && !allDone.load(); ++i) {
        if (!callerExec.runOnce()) {
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
        allDone.store(settled.load() == kBinds);
    }
    REQUIRE(allDone.load());
    for (int i = 0; i < kBinds; ++i) {
        CHECK(bound[static_cast<std::size_t>(i)] == morph::exec::detail::ModelId{static_cast<std::uint64_t>(100 + i)});
    }
}

TEST_CASE("SocketBackend: a bind on a dropped connection is rejected rather than left unsettled",
          "[net][socket_backend][registration-surface]") {
    morph::exec::MainThreadExecutor callerExec;
    std::string error;
    std::atomic<bool> done{false};
    auto capture = [&](const std::exception_ptr& exc) {
        try {
            std::rethrow_exception(exc);
        } catch (const std::exception& err) {
            error = err.what();
        }
        done.store(true);
    };

    SECTION("issued while already disconnected") {
        morph::net::SocketBackend::Config cfg;
        cfg.reconnectEnabled = false;
        cfg.connectTimeout = std::chrono::milliseconds{200};
        morph::net::SocketBackend backend{"ws://127.0.0.1:1", cfg};  // nothing listens there
        REQUIRE_FALSE(backend.waitForConnected(std::chrono::milliseconds{300}));
        backend.bindModel(privateBind("SbEchoModel"), callerExec).onErrorDetached(capture);
        REQUIRE(drainUntil(callerExec, done));
        CHECK_THAT(error, Catch::Matchers::ContainsSubstring("disconnected"));
    }

    SECTION("dropped while in flight") {
        auto fake = std::make_unique<FakeWsServer>();
        morph::net::SocketBackend::Config cfg;
        cfg.reconnectEnabled = false;
        morph::net::SocketBackend backend{"ws://127.0.0.1:" + std::to_string(static_cast<unsigned>(fake->port())),
                                          cfg};
        fake->acceptAndHandshake();
        REQUIRE(backend.waitForConnected());

        backend.bindModel(privateBind("SbEchoModel"), callerExec).onErrorDetached(capture);
        REQUIRE(fake->receiveEnvelope().callId != 0U);
        fake.reset();  // the peer goes away without ever replying
        REQUIRE(drainUntil(callerExec, done));
        CHECK_THAT(error, Catch::Matchers::ContainsSubstring("disconnected"));
    }
}

TEST_CASE("SocketBackend: a bind rejected by the server surfaces the server's own message",
          "[net][socket_backend][registration-surface]") {
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto authz = std::make_shared<DenyAllAuthorizer>();
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool, authz);
    morph::net::SocketServer wsServer{*server, 0};
    REQUIRE(wsServer.listen());

    // Declared before `backend` -- see the identical comment on the first
    // TEST_CASE in this file that needed it: a data race on teardown.
    morph::exec::MainThreadExecutor callerExec;
    morph::net::SocketBackend backend{"ws://127.0.0.1:" + std::to_string(static_cast<unsigned>(wsServer.port()))};
    REQUIRE(backend.waitForConnected());

    std::string error;
    std::atomic<bool> done{false};
    backend.bindModel(privateBind("SbEchoModel"), callerExec).onErrorDetached([&](const std::exception_ptr& exc) {
        try {
            std::rethrow_exception(exc);
        } catch (const std::exception& err) {
            error = err.what();
        }
        done.store(true);
    });
    REQUIRE(drainUntil(callerExec, done));
    // Same "<verb> failed: <server message>" wording the blocking verbs use.
    CHECK_THAT(error, Catch::Matchers::ContainsSubstring("register failed: unauthorized"));
}

TEST_CASE("SocketBackend: a reconnect handler can re-bind through the structural surface without waiting",
          "[net][socket_backend][disconnect][registration-surface]") {
    // The shape a reconnect handler takes once its caller is on the structural
    // surface: issue the bind, attach a continuation, return. The
    // handler parks on nothing, so it has no reply to wait for and cannot hold
    // up whichever thread runs it.
    // The handler's own executor is the bind's owner: the reply is delivered
    // where the continuation was attached.
    ReconnectFixture fixture;
    std::atomic<bool> handlerRan{false};
    std::atomic<bool> reboundOk{false};

    fixture.bounce([&] {
        handlerRan.store(true);
        fixture.backend->bindModel(privateBind("SbEchoModel"), fixture.handlerExec)
            .thenDetached([&](morph::exec::detail::ModelId mid) { reboundOk.store(mid.v != 0U); });
    });

    spinUntil([&] { return reboundOk.load(); }, 500);
    CHECK(handlerRan.load());
    CHECK(reboundOk.load());

    // The transport is unharmed: the synchronous channel was never involved.
    CHECK(fixture.backend->registerModel("SbEchoModel", nullptr).v != 0U);
}

// ── contextKey on a private registration ────────────────────────────────────
//
// `RemoteServer::attachLogIfConfigured` (core/remote.hpp) returns *without
// consulting its `LogProvider` at all* when the envelope's `contextKey` is
// empty, so dropping the key on the way out is not "a log missing its entity
// key" -- the instance is never journalled. Both of this backend's
// private-registration edges therefore have to put the key on the wire: the
// native `bindModel` path and the blocking `registerModelWithContext`. The
// shared/attach shapes always carried it.
//
// Mutation check (AGENTS.md, "would this check still pass if the feature did
// nothing"): every assertion below is on the provider having been consulted
// with the key, or on the resulting journal entry's `entityKey` -- never on the
// registration merely succeeding, which it did before the fix too. Measured:
// with `makeRegister(request.typeId)` restored at the `bindModel` call site the
// first section fails on `requestedFor`, and with
// `registerModelWithContext`'s override removed the second fails the same way.
TEST_CASE("SocketBackend: a private registration carries contextKey to the server's log provider",
          "[net][socket_backend][registration-surface][action_log]") {
    morph::exec::ThreadPoolExecutor serverPool{2};
    // The provider runs on the server's own strand, the assertions on this
    // thread; the mutex is what makes that handoff a data race TSan will not
    // flag rather than one it will.
    std::mutex providerMtx;
    std::vector<std::string> requestedFor;
    auto log = std::make_shared<morph::journal::InMemoryActionLog>();
    morph::backend::ServerConfig serverConfig;
    serverConfig.logProvider = [&](std::string_view modelType, std::string_view contextKey) {
        std::scoped_lock const lock{providerMtx};
        requestedFor.emplace_back(std::string{modelType} + ":" + std::string{contextKey});
        return log;
    };
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool, serverConfig);

    morph::net::SocketServer wsServer{*server, 0};
    REQUIRE(wsServer.listen());

    // Declared before `backend` so it outlives it -- see the ordering note on
    // the first registration-surface test above.
    morph::exec::MainThreadExecutor callerExec;
    morph::net::SocketBackend backend{"ws://127.0.0.1:" + std::to_string(static_cast<unsigned>(wsServer.port()))};
    REQUIRE(backend.waitForConnected());

    SECTION("through the native bindModel path, and the attached log journals under that key") {
        morph::exec::detail::ModelId bound{};
        std::atomic<bool> done{false};
        backend
            .bindModel(morph::backend::detail::BindRequest{.typeId = "SbEchoModel",
                                                           .factory = nullptr,
                                                           .contextKey = "acct-587",
                                                           .primary = {},
                                                           .current = {}},
                       callerExec)
            .thenDetached([&](morph::exec::detail::ModelId mid) {
                bound = mid;
                done.store(true);
            });
        REQUIRE(drainUntil(callerExec, done));
        REQUIRE(bound.v != 0U);
        {
            std::scoped_lock const lock{providerMtx};
            CHECK(requestedFor == std::vector<std::string>{"SbEchoModel:acct-587"});
        }

        // …and the log the provider handed over is really attached to that
        // instance: an action executed against it lands in the journal under
        // the same key. Hand-built `ActionCall`, so the call reaches the
        // instance without a `Bridge` in between.
        morph::backend::detail::ActionCall call{
            .modelTypeId = "SbEchoModel",
            .actionTypeId = "SbEchoAction",
            .serializeAction = [](const void*) { return std::string{R"({"value":7})"}; },
            .deserializeResult =
                [](std::string_view body) {
                    return std::static_pointer_cast<void>(std::make_shared<std::string>(body));
                },
            .localOp = nullptr,
            .session = {},
        };
        morph::exec::MainThreadExecutor cbOwner;
        std::atomic<bool> settled{false};
        backend.execute(bound, std::move(call), &cbOwner)
            .then([&](const std::shared_ptr<void>&) { settled.store(true); })
            .onError([&](const std::exception_ptr&) { settled.store(true); });
        spinUntil(cbOwner, [&] { return settled.load(); });
        REQUIRE(settled.load());

        auto entries = log->entries();
        REQUIRE(entries.size() == 1);
        CHECK(entries[0].entityKey == "acct-587");
        CHECK(entries[0].actionType == "SbEchoAction");
    }

    SECTION("through the blocking registerModelWithContext path") {
        auto const mid = backend.registerModelWithContext("SbEchoModel", nullptr, "acct-blocking");
        REQUIRE(mid.v != 0U);
        std::scoped_lock const lock{providerMtx};
        CHECK(requestedFor == std::vector<std::string>{"SbEchoModel:acct-blocking"});
    }

    SECTION("while plain registerModel still sends no key, so the provider is not consulted") {
        auto const mid = backend.registerModel("SbEchoModel", nullptr);
        REQUIRE(mid.v != 0U);
        std::scoped_lock const lock{providerMtx};
        CHECK(requestedFor.empty());
    }
}

// ── One I/O loop owns the transport ─────────────────────────────────────────
//
// Every piece of `SocketBackend` state lives on the `IoLoop` it was built on.
// Each case below calls verbs from this test's own thread -- never the loop's
// -- and reads, from inside the body each verb posts, whether that body runs
// as a task of the loop (`OwnerProbeRecorder`). A body run inline on the
// calling thread reports `onOwner == false`, and the case fails.

namespace {

bool onLoop(morph::exec::IoLoop& loop) {
    return morph::exec::runningOn(static_cast<core::async::IExecutor const&>(loop.loop()));
}

/// A `RemoteServer` behind a `SocketServer`, and one backend connected to it,
/// all on one `IoLoop`. Members in teardown order: backend, then server, then
/// the loop last.
struct SharedLoopStack {
    morph::exec::IoLoop loop;
    morph::exec::ThreadPoolExecutor serverPool{2};
    std::shared_ptr<morph::backend::RemoteServer> server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    morph::net::SocketServer wsServer{loop, *server, 0};
    std::unique_ptr<morph::net::SocketBackend> backend;

    explicit SharedLoopStack(morph::net::SocketBackend::Config cfg = {}) {
        if (!wsServer.listen()) {
            throw std::runtime_error("SharedLoopStack: listen failed");
        }
        backend = std::make_unique<morph::net::SocketBackend>(loop, url(), cfg);
        if (!backend->waitForConnected()) {
            throw std::runtime_error("SharedLoopStack: connect failed");
        }
    }

    [[nodiscard]] std::string url() const {
        return "ws://127.0.0.1:" + std::to_string(static_cast<unsigned>(wsServer.port()));
    }
};

/// An `SbEchoAction{7}` call, serialised by hand: these cases need a wire
/// round trip, not a typed handler.
morph::backend::detail::ActionCall echoCall() {
    morph::backend::detail::ActionCall call;
    call.modelTypeId = "SbEchoModel";
    call.actionTypeId = "SbEchoAction";
    call.serializeAction = [](const void*) { return std::string{R"({"value":7})"}; };
    call.deserializeResult = [](std::string_view json) -> std::shared_ptr<void> {
        return std::make_shared<std::string>(json);
    };
    return call;
}

/// Waits for @p comp to settle, either way.
template <class T>
bool settles(morph::async::Completion<T>& comp) {
    auto done = std::make_shared<std::atomic<bool>>(false);
    comp.then([done](const T&) { done->store(true); }).onError([done](const std::exception_ptr&) {
        done->store(true);
    });
    return morph::testing::waitUntil([done] { return done->load(); });
}

}  // namespace

TEST_CASE("SocketBackend: every socket write is made on the loop, whichever thread asked",
          "[net][socket_backend][owner]") {
    SharedLoopStack stack;
    morph::testing::OwnerProbeRecorder const recorder{stack.loop.loop()};
    REQUIRE_FALSE(onLoop(stack.loop));

    // A synchronous control call, a fire-and-forget one and an execute, all
    // from this thread.
    auto const mid = stack.backend->registerModel("SbEchoModel", nullptr);
    REQUIRE(mid.v != 0U);
    morph::exec::ThreadPoolExecutor cbPool{1};
    auto comp = stack.backend->execute(mid, echoCall(), &cbPool);
    REQUIRE(settles(comp));
    stack.backend->deregisterModel(mid);
    stack.loop.runAndWait([] {});

    CHECK(recorder.allPosted("SocketBackend::bindModel"));
    CHECK(recorder.allPosted("SocketBackend::execute"));
    CHECK(recorder.allPosted("SocketBackend::deregisterModel"));
    CHECK(recorder.count("SocketBackend::send") >= 3U);
    CHECK(recorder.allPosted("SocketBackend::send"));
}

TEST_CASE("SocketBackend: pending calls are filed, matched and swept on the loop", "[net][socket_backend][owner]") {
    SharedLoopStack stack;
    morph::testing::OwnerProbeRecorder const recorder{stack.loop.loop()};
    morph::exec::ThreadPoolExecutor cbPool{1};

    auto const mid = stack.backend->registerModel("SbEchoModel", nullptr);
    auto comp = stack.backend->execute(mid, echoCall(), &cbPool);
    REQUIRE(settles(comp));
    auto bound = stack.backend->bindModel(privateBind("SbEchoModel"), cbPool);
    REQUIRE(settles(bound));
    stack.backend->cancelPending(std::make_exception_ptr(std::runtime_error{"swept"}));
    stack.loop.runAndWait([] {});

    CHECK(recorder.allPosted("SocketBackend::execute"));
    CHECK(recorder.allPosted("SocketBackend::bindModel"));
    CHECK(recorder.allPosted("SocketBackend::reply"));
    CHECK(recorder.allPosted("SocketBackend::cancelPending"));
}

TEST_CASE("SocketBackend: waitForConnected registers its waiter on the loop", "[net][socket_backend][owner]") {
    morph::exec::IoLoop loop;
    morph::testing::OwnerProbeRecorder const recorder{loop.loop()};
    FakeWsServer fake;
    morph::net::SocketBackend backend{loop, "ws://127.0.0.1:" + std::to_string(static_cast<unsigned>(fake.port()))};

    // Nothing has accepted yet, so this waits: its waiter is filed on the loop
    // and released by the timeout.
    CHECK_FALSE(backend.waitForConnected(std::chrono::milliseconds{50}));
    fake.acceptAndHandshake();
    CHECK(backend.waitForConnected());

    CHECK(recorder.allPosted("SocketBackend::waitForConnected"));
}

TEST_CASE("SocketBackend: the reconnect backoff is armed on the loop", "[net][socket_backend][owner][disconnect]") {
    morph::exec::IoLoop loop;
    morph::testing::OwnerProbeRecorder const recorder{loop.loop()};
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    morph::net::SocketBackend::Config cfg;
    cfg.initialReconnectDelay = std::chrono::milliseconds{20};
    cfg.maxReconnectDelay = std::chrono::milliseconds{50};

    auto wsServer = std::make_unique<morph::net::SocketServer>(loop, *server, 0);
    REQUIRE(wsServer->listen());
    auto const port = wsServer->port();
    morph::net::SocketBackend backend{loop, "ws://127.0.0.1:" + std::to_string(static_cast<unsigned>(port)), cfg};
    REQUIRE(backend.waitForConnected());

    wsServer.reset();  // the backend sees the drop and backs off
    REQUIRE(morph::testing::waitUntil([&] { return recorder.count("SocketBackend::reconnect") >= 1U; }));
    wsServer = std::make_unique<morph::net::SocketServer>(loop, *server, port);
    REQUIRE(wsServer->listen());
    REQUIRE(morph::testing::waitUntil([&] { return backend.waitForConnected(std::chrono::milliseconds{50}); }));

    CHECK(recorder.allPosted("SocketBackend::reconnect"));
}

TEST_CASE("SocketBackend: a timer and a connected socket share one loop, and the timer fires",
          "[net][socket_backend][owner]") {
    SharedLoopStack stack;
    morph::async::detail::TimeoutScheduler scheduler{stack.loop};
    std::atomic<bool> timerOnLoop{false};
    std::atomic<bool> fired{false};
    std::atomic<bool> connectedWhenFired{false};

    scheduler.schedule(std::chrono::milliseconds{20}, [&] {
        timerOnLoop = onLoop(stack.loop);
        connectedWhenFired = stack.backend->waitForConnected(std::chrono::milliseconds{0});
        fired = true;
    });
    REQUIRE(morph::testing::waitUntil([&] { return fired.load(); }));
    CHECK(timerOnLoop.load());
    CHECK(connectedWhenFired.load());

    // And the socket still carries a round trip on the same loop.
    auto const mid = stack.backend->registerModel("SbEchoModel", nullptr);
    morph::exec::MainThreadExecutor cbOwner;
    std::atomic<bool> replied{false};
    auto comp = stack.backend->execute(mid, echoCall(), &cbOwner);
    comp.then([&](const std::shared_ptr<void>&) { replied = true; }).onError([](const std::exception_ptr&) {});
    CHECK(morph::testing::pumpOwnerUntil(cbOwner, [&] { return replied.load(); }));
}

TEST_CASE("SocketBackend: destroyed on the loop thread, from a timer callback, it does not deadlock",
          "[net][socket_backend][owner]") {
    SharedLoopStack stack;
    morph::async::detail::TimeoutScheduler scheduler{stack.loop};
    std::atomic<bool> destroyed{false};
    std::atomic<bool> destroyedOnLoop{false};

    scheduler.schedule(std::chrono::milliseconds{1}, [&] {
        destroyedOnLoop = onLoop(stack.loop);
        stack.backend.reset();
        destroyed = true;
    });
    REQUIRE(morph::testing::waitUntil([&] { return destroyed.load(); },
                                      morph::testing::WaitBudget{std::chrono::seconds{5}}));
    CHECK(destroyedOnLoop.load());
}

TEST_CASE("SocketBackend: destroyed inside its own reply's continuation, on the loop, it does not deadlock",
          "[net][socket_backend][owner]") {
    // The continuation runs on the inline executor, so it runs inside the
    // backend's own reply flow: the destructor closes the backend under the
    // flow that is delivering to it, and the flow must notice and stop.
    SharedLoopStack stack;
    auto const mid = stack.backend->registerModel("SbEchoModel", nullptr);
    std::atomic<bool> destroyed{false};
    std::atomic<bool> destroyedOnLoop{false};

    auto comp = stack.backend->execute(mid, echoCall(), &morph::exec::detail::inlineExecutor());
    comp.then([&](const std::shared_ptr<void>&) {
            destroyedOnLoop = onLoop(stack.loop);
            stack.backend.reset();
            destroyed = true;
        })
        .onError([](const std::exception_ptr&) {});
    REQUIRE(morph::testing::waitUntil([&] { return destroyed.load(); },
                                      morph::testing::WaitBudget{std::chrono::seconds{5}}));
    CHECK(destroyedOnLoop.load());
}

TEST_CASE("SocketBackend: the session and the reconnect handler are stored on the loop, whichever thread set them",
          "[net][socket_backend][owner]") {
    SharedLoopStack stack;
    morph::testing::OwnerProbeRecorder const recorder{stack.loop.loop()};
    morph::exec::ThreadPoolExecutor handlerExec{1};

    stack.backend->setSession(morph::session::Context{.principal = "alice"});
    stack.backend->setReconnectHandler([] {}, &handlerExec);
    // A synchronous verb issues the loop request its asynchronous twin does and
    // waits: the waiting is on this thread, the work on the loop.
    REQUIRE(stack.backend->registerModel("SbEchoModel", nullptr).v != 0U);
    stack.backend->setReconnectHandler(nullptr, nullptr);
    stack.loop.runAndWait([] {});

    CHECK(recorder.allPosted("SocketBackend::setSession"));
    CHECK(recorder.allPosted("SocketBackend::setReconnectHandler"));
    CHECK(recorder.allPosted("SocketBackend::bindModel"));
}

TEST_CASE("SocketBackend: a synchronous verb called on the loop's own thread fails instead of waiting on itself",
          "[net][socket_backend][owner]") {
    SharedLoopStack stack;
    std::string message;
    stack.loop.runAndWait([&] {
        try {
            static_cast<void>(stack.backend->registerModel("SbEchoModel", nullptr));
        } catch (const std::exception& exc) {
            message = exc.what();
        }
    });
    CHECK_THAT(message, Catch::Matchers::ContainsSubstring("cannot wait on the I/O loop's own thread"));
}

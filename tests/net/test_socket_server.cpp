// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <memory>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/model.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <morph/core/wire.hpp>
#include <morph/net/detail/tcp_socket.hpp>
#include <morph/net/detail/ws_frame.hpp>
#include <morph/net/detail/ws_handshake.hpp>
#include <morph/net/socket_server.hpp>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "../test_support.hpp"

// ── Test model, registered process-wide (same pattern as tests/qt/test_qt_websocket.cpp) ──
// Deliberately NOT in an anonymous namespace: glaze's reflection-based
// get_name() needs these types to have external linkage (see
// tests/qt/test_qt_websocket.cpp's WsEchoAction/WsEchoModel for the same
// convention).
struct NetEchoAction {
    int value = 0;
};
struct NetEchoFail {};

struct NetEchoModel {
    int execute(NetEchoAction action) { return action.value; }
    int execute(NetEchoFail) { throw std::runtime_error("echo failed"); }
};

BRIDGE_REGISTER_MODEL(NetEchoModel, "NetEchoModel")
BRIDGE_REGISTER_ACTION(NetEchoModel, NetEchoAction, "NetEchoAction")
BRIDGE_REGISTER_ACTION(NetEchoModel, NetEchoFail, "NetEchoFail")

namespace {

// ── Minimal manual raw-WebSocket client, built directly from the detail::
// helpers. Deliberately does NOT use SocketBackend (built in Task 8) so this
// test isolates SocketServer's correctness.
class RawWsClient {
public:
    explicit RawWsClient(std::uint16_t port) {
        _socket = morph::net::detail::TcpSocket::connect("127.0.0.1", port, std::chrono::milliseconds{2000});
        morph::net::detail::ParsedWsUrl url{"127.0.0.1", port, "/"};
        std::string leftover = morph::net::detail::performClientHandshake(_socket, url);
        _reader.feed(leftover);
    }

    void send(const morph::wire::Envelope& env) {
        std::string frame = morph::net::detail::encodeWsFrame(morph::net::detail::WsOpcode::kText,
                                                              morph::wire::encode(env), /*mask=*/true);
        _socket.sendAll(frame.data(), frame.size());
    }

    morph::wire::Envelope receive() {
        for (;;) {
            if (auto frame = _reader.tryExtractFrame()) {
                return morph::wire::decode(frame->payload);
            }
            char buf[4096];
            std::size_t got = _socket.recvSome(buf, sizeof(buf));
            if (got == 0) {
                throw std::runtime_error("RawWsClient::receive: peer closed");
            }
            _reader.feed(std::string_view{buf, got});
        }
    }

private:
    morph::net::detail::TcpSocket _socket;
    morph::net::detail::WsFrameReader _reader;
};

}  // namespace

TEST_CASE("SocketServer: register -> execute -> reply round trip with a raw client", "[net][socket_server]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);
    morph::net::SocketServer wsServer{*server, 0};
    REQUIRE(wsServer.listen());

    RawWsClient client{wsServer.port()};

    client.send(morph::wire::makeRegister("NetEchoModel"));
    auto regReply = client.receive();
    REQUIRE(regReply.kind == "ok");
    std::uint64_t const modelId = regReply.modelId;
    REQUIRE(modelId != 0U);

    morph::wire::Envelope execReq;
    execReq.kind = "execute";
    execReq.callId = 1;
    execReq.modelId = modelId;
    execReq.modelType = "NetEchoModel";
    execReq.actionType = "NetEchoAction";
    execReq.body = R"({"value":42})";
    client.send(execReq);
    auto execReply = client.receive();
    REQUIRE(execReply.kind == "ok");
    REQUIRE(execReply.callId == 1U);
    REQUIRE(execReply.body == "42");

    client.send(morph::wire::makeDeregister(modelId));
    auto deregReply = client.receive();
    REQUIRE(deregReply.kind == "ok");
}

TEST_CASE("SocketServer: concurrent in-flight executes are matched by callId", "[net][socket_server]") {
    morph::exec::ThreadPoolExecutor pool{4};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);
    morph::net::SocketServer wsServer{*server, 0};
    REQUIRE(wsServer.listen());

    RawWsClient client{wsServer.port()};
    client.send(morph::wire::makeRegister("NetEchoModel"));
    auto regReply = client.receive();
    REQUIRE(regReply.kind == "ok");
    std::uint64_t const modelId = regReply.modelId;

    constexpr int numCalls = 10;
    for (int i = 1; i <= numCalls; ++i) {
        morph::wire::Envelope req;
        req.kind = "execute";
        req.callId = static_cast<std::uint64_t>(i);
        req.modelId = modelId;
        req.modelType = "NetEchoModel";
        req.actionType = "NetEchoAction";
        req.body = R"({"value":)" + std::to_string(i) + "}";
        client.send(req);
    }
    std::vector<bool> seen(static_cast<std::size_t>(numCalls) + 1, false);
    for (int i = 0; i < numCalls; ++i) {
        auto reply = client.receive();
        REQUIRE(reply.kind == "ok");
        REQUIRE(reply.callId >= 1U);
        REQUIRE(reply.callId <= static_cast<std::uint64_t>(numCalls));
        REQUIRE(reply.body == std::to_string(reply.callId));
        seen[reply.callId] = true;
    }
    for (int i = 1; i <= numCalls; ++i) {
        REQUIRE(seen[static_cast<std::size_t>(i)]);
    }
}

TEST_CASE("SocketServer: two clients share one server with isolated model state", "[net][socket_server]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);
    morph::net::SocketServer wsServer{*server, 0};
    REQUIRE(wsServer.listen());

    RawWsClient clientA{wsServer.port()};
    RawWsClient clientB{wsServer.port()};

    clientA.send(morph::wire::makeRegister("NetEchoModel"));
    auto regA = clientA.receive();
    clientB.send(morph::wire::makeRegister("NetEchoModel"));
    auto regB = clientB.receive();
    REQUIRE(regA.modelId != regB.modelId);

    morph::wire::Envelope reqA;
    reqA.kind = "execute";
    reqA.callId = 1;
    reqA.modelId = regA.modelId;
    reqA.modelType = "NetEchoModel";
    reqA.actionType = "NetEchoAction";
    reqA.body = R"({"value":10})";
    clientA.send(reqA);
    auto replyA = clientA.receive();
    REQUIRE(replyA.body == "10");

    morph::wire::Envelope reqB = reqA;
    reqB.modelId = regB.modelId;
    reqB.body = R"({"value":20})";
    clientB.send(reqB);
    auto replyB = clientB.receive();
    REQUIRE(replyB.body == "20");
}

TEST_CASE("SocketServer: an action exception surfaces as an err reply, connection stays usable",
          "[net][socket_server]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);
    morph::net::SocketServer wsServer{*server, 0};
    REQUIRE(wsServer.listen());

    RawWsClient client{wsServer.port()};
    client.send(morph::wire::makeRegister("NetEchoModel"));
    auto regReply = client.receive();

    morph::wire::Envelope failReq;
    failReq.kind = "execute";
    failReq.callId = 5;
    failReq.modelId = regReply.modelId;
    failReq.modelType = "NetEchoModel";
    failReq.actionType = "NetEchoFail";
    failReq.body = "{}";
    client.send(failReq);
    auto failReply = client.receive();
    REQUIRE(failReply.kind == "err");
    REQUIRE(failReply.callId == 5U);

    // The connection must still work after an error reply.
    morph::wire::Envelope okReq;
    okReq.kind = "execute";
    okReq.callId = 6;
    okReq.modelId = regReply.modelId;
    okReq.modelType = "NetEchoModel";
    okReq.actionType = "NetEchoAction";
    okReq.body = R"({"value":7})";
    client.send(okReq);
    auto okReply = client.receive();
    REQUIRE(okReply.kind == "ok");
    REQUIRE(okReply.body == "7");
}

// ── Connection-scoped reclamation over the raw-socket transport ─────────────
// The scope machinery (RemoteServer::openConnection/closeConnection) was
// originally wired into QtWebSocketServer only; SocketServer dispatched through
// the unscoped two-argument handle(), so every model it registered outlived its
// connection forever. These pin the raw-socket transport's half of it.

TEST_CASE("SocketServer: dropping a client reclaims the models it registered", "[net][socket_server]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);
    morph::net::SocketServer wsServer{*server, 0};
    REQUIRE(wsServer.listen());

    std::uint64_t modelId = 0;
    {
        RawWsClient client{wsServer.port()};
        client.send(morph::wire::makeRegister("NetEchoModel"));
        auto regReply = client.receive();
        REQUIRE(regReply.kind == "ok");
        modelId = regReply.modelId;
        REQUIRE(modelId != 0U);
        REQUIRE(server->health().liveModels == 1U);
    }  // client destructs: the socket closes and the server observes EOF

    REQUIRE(morph::testing::waitUntil([&] { return server->health().liveModels == 0U; }, std::chrono::seconds{5}));

    // A late execute against the reclaimed id is answered, not serviced.
    RawWsClient probe{wsServer.port()};
    morph::wire::Envelope execReq;
    execReq.kind = "execute";
    execReq.callId = 1;
    execReq.modelId = modelId;
    execReq.modelType = "NetEchoModel";
    execReq.actionType = "NetEchoAction";
    execReq.body = R"({"value":42})";
    probe.send(execReq);
    auto execReply = probe.receive();
    REQUIRE(execReply.kind == "err");
    REQUIRE(execReply.message == "model not found");
}

TEST_CASE("SocketServer: each client's models are reclaimed independently", "[net][socket_server]") {
    morph::exec::ThreadPoolExecutor pool{4};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);
    morph::net::SocketServer wsServer{*server, 0};
    REQUIRE(wsServer.listen());

    RawWsClient survivor{wsServer.port()};
    survivor.send(morph::wire::makeRegister("NetEchoModel"));
    auto survivorReg = survivor.receive();
    REQUIRE(survivorReg.kind == "ok");

    {
        RawWsClient transient{wsServer.port()};
        transient.send(morph::wire::makeRegister("NetEchoModel"));
        REQUIRE(transient.receive().kind == "ok");
        REQUIRE(server->health().liveModels == 2U);
    }

    // Only the departed client's instance goes; the survivor keeps working.
    REQUIRE(morph::testing::waitUntil([&] { return server->health().liveModels == 1U; }, std::chrono::seconds{5}));

    morph::wire::Envelope execReq;
    execReq.kind = "execute";
    execReq.callId = 1;
    execReq.modelId = survivorReg.modelId;
    execReq.modelType = "NetEchoModel";
    execReq.actionType = "NetEchoAction";
    execReq.body = R"({"value":7})";
    survivor.send(execReq);
    auto execReply = survivor.receive();
    REQUIRE(execReply.kind == "ok");
    REQUIRE(execReply.body == "7");
}

TEST_CASE("SocketServer::close() reclaims every connected client's models", "[net][socket_server]") {
    morph::exec::ThreadPoolExecutor pool{4};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);
    auto wsServer = std::make_unique<morph::net::SocketServer>(*server, 0);
    REQUIRE(wsServer->listen());

    RawWsClient clientA{wsServer->port()};
    clientA.send(morph::wire::makeRegister("NetEchoModel"));
    REQUIRE(clientA.receive().kind == "ok");
    RawWsClient clientB{wsServer->port()};
    clientB.send(morph::wire::makeRegister("NetEchoModel"));
    REQUIRE(clientB.receive().kind == "ok");
    REQUIRE(server->health().liveModels == 2U);

    // close() joins the client threads, each of which runs its own scope
    // teardown on the way out.
    wsServer->close();
    REQUIRE(server->health().liveModels == 0U);
}

// The Catch2 assertion macros, not branching logic, are what push this over the
// cognitive-complexity threshold -- as in the sibling cases above.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("SocketServer: two threads calling close() concurrently do not both reach join()", "[net][socket_server]") {
    // Regression coverage for morph#451. close() guarded itself with
    //
    //     bool const wasAlreadyClosing = _closing.exchange(true);
    //     if (wasAlreadyClosing && !_acceptThread.joinable()) { return; }
    //
    // which is not a mutual exclusion: the loser of the exchange still sees a
    // *joinable* accept thread (the winner has not joined it yet, and cannot
    // have -- it is still parked in poll() until the winner's wakeup byte
    // releases it), falls through, and calls join() on the very same
    // std::thread the winner is joining. Two concurrent join()s on one
    // thread is UB, and the two platforms cash it out differently: on
    // Linux/glibc the loser parks forever in pthread_join's futex on a thread
    // descriptor the winner already reaped, on macOS/libc++ it throws
    // std::system_error. Serialising the whole body on a dedicated mutex makes
    // the second caller wait and then observe !joinable(), which is what the
    // atomic exchange was already trying, and failing, to express.
    //
    // The try/catch matters as much as the ctest TIMEOUT does: this test has
    // to fail on both of those outcomes, and asserting only "nothing was
    // thrown" would go green on Linux with the bug present -- the hang is
    // caught by the timeout, the exception by the catch.
    //
    // Deliberately scoped to close()-vs-close() on a *live* server.
    // close()-vs-destruction is out of contract (see
    // docs/spec/concurrency_and_lifetimes.md, "Destruction ordering") and is
    // not fixable here anyway: member destruction would destroy the mutex
    // itself out from under a caller still blocked on it.
    constexpr int kIterations = 20;
    for (int iter = 0; iter < kIterations; ++iter) {
        morph::exec::ThreadPoolExecutor pool{2};
        auto server = std::make_shared<morph::backend::RemoteServer>(pool);
        morph::net::SocketServer wsServer{*server, 0};
        REQUIRE(wsServer.listen());

        // Spin barrier rather than a sleep: the window is a handful of
        // instructions between the exchange and the join, so both racers have
        // to be released as close to simultaneously as the scheduler allows.
        std::atomic<int> ready{0};
        std::atomic<bool> released{false};
        std::atomic<int> threw{0};
        std::atomic<int> returned{0};

        std::vector<std::thread> racers;
        racers.reserve(2);
        for (int racer = 0; racer < 2; ++racer) {
            racers.emplace_back([&] {
                ready.fetch_add(1);
                while (!released.load()) {
                    // busy-wait, deliberately: yielding here would let one
                    // racer finish close() before the other is scheduled.
                }
                try {
                    wsServer.close();
                } catch (...) {
                    threw.fetch_add(1);
                }
                returned.fetch_add(1);
            });
        }
        while (ready.load() != 2) {
            std::this_thread::yield();
        }
        released.store(true);

        // Pre-fix this join is where the run stops: the losing close() never
        // returns, so its thread is never joinable-complete.
        for (auto& racer : racers) {
            racer.join();
        }

        REQUIRE(threw.load() == 0);
        REQUIRE(returned.load() == 2);
    }
}

// The Catch2 assertion macros, not branching logic, are what push this over the
// cognitive-complexity threshold -- as in the sibling cases above.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("SocketServer: tearing down a parked accept loop finishes promptly", "[net][socket_server]") {
    // Regression coverage for morph#437. close() used to interrupt the accept
    // thread with `_listenSocket.shutdownBoth()` -- ::shutdown(fd, SHUT_RDWR)
    // on the *listening* socket -- and then join() it. That works only because
    // Linux chooses to kick a parked accept(2) when its listener is shut down.
    // It is not a POSIX guarantee and macOS/BSD do not do it, so on those
    // kernels the accept thread stayed parked, join() never returned, and
    // ~SocketServer() hung until something external killed the process.
    //
    // Why this can fail *here*, on Linux, which is the whole point of writing
    // it (AGENTS.md, "would this still pass if the feature did nothing?"): the
    // fix does not add a second wakeup beside the kernel's, it *replaces* it.
    // close() no longer touches the listening socket at all, so the pipe write
    // is now the only thing that ends the loop on every platform. Delete that
    // one write() and this test hangs on Linux exactly as the shutdown-based
    // teardown hung on Darwin -- which is the falsification the PR records.
    //
    // No client, pending or established: the accept thread has to be genuinely
    // parked in poll() with nothing else that could return it.
    constexpr auto kBudget = std::chrono::seconds{2};

    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);
    auto wsServer = std::make_unique<morph::net::SocketServer>(*server, 0);
    REQUIRE(wsServer->listen());
    REQUIRE(wsServer->port() != 0U);

    // Let the accept thread actually reach its wait. Without this the test
    // could measure a loop that had not started, which is not the parked case.
    std::this_thread::sleep_for(std::chrono::milliseconds{100});

    auto const closeStart = std::chrono::steady_clock::now();
    wsServer->close();
    auto const closeElapsed = std::chrono::steady_clock::now() - closeStart;

    // A bounded time, not "we reached this line": reaching the next line only
    // proves the process was not killed, and cannot tell a prompt teardown from
    // one that stalled for a minute under ctest's 120s cap.
    REQUIRE(closeElapsed < kBudget);

    // And the same for destruction, which is how every other test in this file
    // (and every application) actually tears a server down.
    auto const dtorStart = std::chrono::steady_clock::now();
    wsServer.reset();
    auto const dtorElapsed = std::chrono::steady_clock::now() - dtorStart;
    REQUIRE(dtorElapsed < kBudget);
}

TEST_CASE("SocketServer: a parked accept loop survives repeated listen/close cycles", "[net][socket_server]") {
    // The wakeup pipe is per-listen(): each cycle must get a fresh one, or the
    // byte the previous close() left undrained would make the next accept loop
    // return the instant it started -- a server that binds a port and then
    // silently refuses to accept anything, which no timing assertion above
    // would catch.
    constexpr auto kBudget = std::chrono::seconds{2};
    constexpr int kCycles = 5;

    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);
    morph::net::SocketServer wsServer{*server, 0};

    for (int cycle = 0; cycle < kCycles; ++cycle) {
        REQUIRE(wsServer.listen());
        REQUIRE(wsServer.port() != 0U);

        // Proof the loop is still accepting on this cycle, not merely running:
        // a real client completes the handshake against it.
        RawWsClient client{wsServer.port()};
        client.send(morph::wire::makeRegister("NetEchoModel"));
        REQUIRE(client.receive().kind == "ok");

        auto const started = std::chrono::steady_clock::now();
        wsServer.close();
        REQUIRE(std::chrono::steady_clock::now() - started < kBudget);
    }
}

TEST_CASE("SocketServer::close() releases the listening port", "[net][socket_server]") {
    // close() stops calling shutdownBoth() on the listener (morph#437), so it
    // has to drop the descriptor instead -- left open with no accept thread,
    // the kernel would keep completing handshakes into a backlog nobody drains
    // and a client would hang in the WebSocket Upgrade read rather than fail
    // fast. Same post-close observation QtWebSocketServer already makes.
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);
    morph::net::SocketServer wsServer{*server, 0};
    REQUIRE(wsServer.listen());
    std::uint16_t const port = wsServer.port();
    REQUIRE(port != 0U);

    wsServer.close();
    REQUIRE(wsServer.port() == 0U);

    // The port is genuinely free again, not merely forgotten.
    morph::net::SocketServer rebound{*server, port};
    REQUIRE(rebound.listen());
    REQUIRE(rebound.port() == port);
}

TEST_CASE("SocketServer: teardown racing a connecting client still finishes promptly", "[net][socket_server]") {
    // The interleaving the parked-loop case above deliberately excludes: a
    // client arriving while close() is already under way, so the accept loop
    // may be anywhere between poll(), tryAccept(), and spawning a client
    // thread. Complements the parked case; it does not replace it, because a
    // pending connection is itself something that returns poll().
    constexpr auto kBudget = std::chrono::seconds{5};
    constexpr int kIterations = 10;

    for (int iter = 0; iter < kIterations; ++iter) {
        morph::exec::ThreadPoolExecutor pool{2};
        auto server = std::make_shared<morph::backend::RemoteServer>(pool);
        morph::net::SocketServer wsServer{*server, 0};
        REQUIRE(wsServer.listen());
        std::uint16_t const port = wsServer.port();

        std::thread connector{[port] {
            try {
                auto sock = morph::net::detail::TcpSocket::connect("127.0.0.1", port, std::chrono::milliseconds{500});
                static_cast<void>(sock);
            } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch) — see below
                // Refused because teardown won the race -- the expected outcome
                // half the time, and not what this test is measuring.
            }
        }};

        auto const started = std::chrono::steady_clock::now();
        wsServer.close();
        auto const elapsed = std::chrono::steady_clock::now() - started;
        connector.join();
        REQUIRE(elapsed < kBudget);
    }
}

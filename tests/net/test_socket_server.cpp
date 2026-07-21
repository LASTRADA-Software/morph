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

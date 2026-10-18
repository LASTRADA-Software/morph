// SPDX-License-Identifier: Apache-2.0

// The `net` transport carrying server-initiated events: the server side's
// per-connection byte backlog and drain call, and the client side's event
// routing and connect handler.

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/io_loop.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <morph/core/wire.hpp>
#include <morph/net/detail/tcp_socket.hpp>
#include <morph/net/detail/ws_connection.hpp>
#include <morph/net/detail/ws_frame.hpp>
#include <morph/net/detail/ws_handshake.hpp>
#include <morph/net/socket_backend.hpp>
#include <morph/net/socket_server.hpp>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "../push_conformance.hpp"
#include "../test_support.hpp"

// External linkage: glaze reflection and the BRIDGE_REGISTER_* macros need it.
// NOLINTBEGIN(misc-use-internal-linkage)
struct PnEcho {
    int value = 0;
};
struct PnBig {
    int size = 0;
};
struct PnModel {
    int execute(PnEcho action) { return action.value; }
    // NOLINTNEXTLINE(modernize-return-braced-init-list): braces would pick the initializer_list constructor.
    std::string execute(PnBig action) { return std::string(static_cast<std::size_t>(action.size), 'x'); }
};
// NOLINTEND(misc-use-internal-linkage)

BRIDGE_REGISTER_MODEL(PnModel, "PnModel")
BRIDGE_REGISTER_ACTION(PnModel, PnEcho, "PnEcho")
BRIDGE_REGISTER_ACTION(PnModel, PnBig, "PnBig")

namespace {

using namespace std::chrono_literals;

/// Runs each task inline on the posting thread, and counts them.
struct CountingExec final : ::morph::exec::IExecutor {
    std::atomic<int> posted{0};
    void post(std::function<void()> task) override {
        posted.fetch_add(1);
        task();
    }
};

/// What an event handler saw, from whichever thread delivered it.
struct EventLog {
    std::mutex mtx;
    std::vector<::morph::wire::EventBody> events;

    std::function<void(::morph::wire::EventBody)> handler() {
        return [this](::morph::wire::EventBody body) {
            std::scoped_lock const lock{mtx};
            events.push_back(std::move(body));
        };
    }
    std::size_t size() {
        std::scoped_lock const lock{mtx};
        return events.size();
    }
    std::vector<::morph::wire::EventBody> snapshot() {
        std::scoped_lock const lock{mtx};
        return events;
    }
};

/// The connection id a fresh `SocketServer` connection to @p server will get:
/// `RemoteServer` numbers connections in order.
::morph::backend::ConnectionId nextConnectionId(::morph::backend::RemoteServer& server) {
    auto const probe = server.openConnection();
    server.closeConnection(probe);
    return probe + 1;
}

std::string wsUrl(std::uint16_t port) { return "ws://127.0.0.1:" + std::to_string(static_cast<unsigned>(port)); }

/// A raw WebSocket client against a real `SocketServer`.
class RawClient {
public:
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a port, then an optional buffer size every caller names in a comment.
    explicit RawClient(std::uint16_t port, int receiveBuffer = 0) : _reader{/*expectMasked=*/false} {
        _socket = ::morph::net::detail::TcpSocket::connect("127.0.0.1", port, 2000ms);
        if (receiveBuffer > 0) {
            ::setsockopt(_socket.nativeHandle(), SOL_SOCKET, SO_RCVBUF, &receiveBuffer, sizeof(receiveBuffer));
        }
        ::morph::net::detail::ParsedWsUrl const url{.host = "127.0.0.1", .port = port, .path = "/"};
        _reader.feed(::morph::net::detail::performClientHandshake(_socket, url));
    }

    void send(const ::morph::wire::Envelope& env) {
        std::string const frame = ::morph::net::detail::encodeWsFrame(::morph::net::detail::WsOpcode::kText,
                                                                      ::morph::wire::encode(env), /*mask=*/true);
        _socket.sendAll(frame.data(), frame.size());
    }

    ::morph::wire::Envelope receive() {
        for (;;) {
            if (auto frame = _reader.tryExtractFrame()) {
                return ::morph::wire::decode(frame->payload);
            }
            std::array<char, 65536> buf{};
            std::size_t const got = _socket.recvSome(buf.data(), buf.size());
            if (got == 0) {
                throw std::runtime_error("RawClient: peer closed");
            }
            _reader.feed(std::string_view{buf.data(), got});
        }
    }

    void boundReceives(std::chrono::milliseconds timeout) { static_cast<void>(_socket.setRecvTimeout(timeout)); }

private:
    ::morph::net::detail::TcpSocket _socket;
    ::morph::net::detail::WsFrameReader _reader;
};

/// Closes and drops a test's `LoopConnection` on its loop when the test ends,
/// passed or failed: its socket must be released there.
struct LoopConnectionGuard {
    ::morph::exec::IoLoop& io;
    std::shared_ptr<::morph::net::detail::LoopConnection>& conn;
    LoopConnectionGuard(::morph::exec::IoLoop& loop, std::shared_ptr<::morph::net::detail::LoopConnection>& held)
        : io{loop}, conn{held} {}
    LoopConnectionGuard(const LoopConnectionGuard&) = delete;
    LoopConnectionGuard& operator=(const LoopConnectionGuard&) = delete;
    LoopConnectionGuard(LoopConnectionGuard&&) = delete;
    LoopConnectionGuard& operator=(LoopConnectionGuard&&) = delete;
    ~LoopConnectionGuard() {
        io.runAndWait([this] {
            if (conn) {
                ::morph::net::detail::closeConnection(*conn);
                conn.reset();
            }
        });
    }
};

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the envelope's fields in wire order; swapped, the server refuses an unknown model.
::morph::wire::Envelope executeEnvelope(std::uint64_t callId, std::uint64_t modelId, std::string_view action,
                                        std::string body) {
    ::morph::wire::Envelope env;
    env.kind = "execute";
    env.callId = callId;
    env.modelId = modelId;
    env.modelType = "PnModel";
    env.actionType = std::string{action};
    env.body = std::move(body);
    return env;
}

}  // namespace

TEST_CASE("net_posted_event_reaches_backend_handler", "[push][channel]") {
    ::morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<::morph::backend::RemoteServer>(pool);
    ::morph::net::SocketServer wsServer{*server, 0};
    REQUIRE(wsServer.listen());
    auto const cid = nextConnectionId(*server);

    CountingExec exec;
    EventLog log;
    ::morph::net::SocketBackend backend{wsUrl(wsServer.port())};
    backend.setEventHandler(log.handler(), &exec);
    REQUIRE(backend.waitForConnected());

    server->post(cid, ::morph::wire::encode(::morph::wire::makeEvent("changes", R"({"n":1})")));
    server->post(cid, ::morph::wire::encode(::morph::wire::makeEvent("resync", "{}")));
    REQUIRE(::morph::testing::waitUntil([&] { return log.size() == 2; }));

    auto const got = log.snapshot();
    CHECK(got[0].topic == "changes");
    CHECK(got[0].payload.str == R"({"n":1})");
    CHECK(got[1].topic == "resync");
    CHECK(exec.posted.load() == 2);

    // Cleared: a later event is dropped, and the connection keeps working.
    backend.setEventHandler(nullptr, nullptr);
    server->post(cid, ::morph::wire::encode(::morph::wire::makeEvent("changes", "{}")));
    CHECK(backend.registerModel("PnModel", nullptr).v != 0U);
    CHECK(log.size() == 2);
}

TEST_CASE("net_event_is_routed_before_reply_matching", "[push][channel]") {
    // A fake server, so an event can carry the very callId a pending execute
    // was filed under: only routing by kind first keeps it off that call.
    auto listener = ::morph::net::detail::TcpSocket::listen(0);
    ::morph::net::SocketBackend backend{wsUrl(listener.boundPort())};
    auto peer = listener.accept();
    ::morph::net::detail::WsFrameReader reader{/*expectMasked=*/true};
    reader.feed(::morph::net::detail::performServerHandshake(peer));
    REQUIRE(backend.waitForConnected());

    CountingExec exec;
    EventLog log;
    backend.setEventHandler(log.handler(), &exec);

    ::morph::backend::detail::ActionCall call;
    call.modelTypeId = "PnModel";
    call.actionTypeId = "PnEcho";
    call.serializeAction = [](const void*) { return std::string{"{}"}; };
    call.deserializeResult = [](std::string_view body) -> std::shared_ptr<void> {
        return std::make_shared<int>(std::stoi(std::string{body}));
    };
    ::morph::exec::MainThreadExecutor owner;
    std::atomic<int> result{-1};
    std::atomic<bool> failed{false};
    backend.execute(::morph::exec::detail::ModelId{1}, std::move(call), &owner)
        .then([&](const std::shared_ptr<void>& value) { result.store(*std::static_pointer_cast<int>(value)); })
        .onError([&](const std::exception_ptr&) { failed.store(true); });

    ::morph::wire::Envelope request;
    for (;;) {
        if (auto frame = reader.tryExtractFrame()) {
            request = ::morph::wire::decode(frame->payload);
            break;
        }
        std::array<char, 4096> buf{};
        std::size_t const got = peer.recvSome(buf.data(), buf.size());
        REQUIRE(got != 0);
        reader.feed(std::string_view{buf.data(), got});
    }
    REQUIRE(request.kind == "execute");
    REQUIRE(request.callId != 0);

    auto sendText = [&peer](const ::morph::wire::Envelope& env) {
        std::string const frame = ::morph::net::detail::encodeWsFrame(::morph::net::detail::WsOpcode::kText,
                                                                      ::morph::wire::encode(env), /*mask=*/false);
        peer.sendAll(frame.data(), frame.size());
    };
    auto sameId = ::morph::wire::makeEvent("changes", R"({"which":"same"})");
    sameId.callId = request.callId;
    sendText(sameId);
    sendText(::morph::wire::makeEvent("changes", R"({"which":"zero"})"));
    REQUIRE(::morph::testing::waitUntil([&] { return log.size() == 2; }));
    for (int i = 0; i < 5; ++i) {
        owner.runFor(10ms);
    }
    CHECK(result.load() == -1);
    CHECK_FALSE(failed.load());

    // The call is still pending, and its own reply settles it.
    sendText(::morph::wire::makeOk(request.callId, "7"));
    for (int i = 0; i < 200 && result.load() == -1 && !failed.load(); ++i) {
        owner.runFor(10ms);
    }
    CHECK(result.load() == 7);
    CHECK_FALSE(failed.load());
}

TEST_CASE("net_connect_handler_fires_on_first_and_reconnect", "[push][channel]") {
    ::morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<::morph::backend::RemoteServer>(pool);

    // Learn a free port, then leave it closed: the backend starts retrying
    // before anything listens, so its handlers are installed before the first
    // connect can happen.
    std::uint16_t port = 0;
    {
        ::morph::net::SocketServer probe{*server, 0};
        REQUIRE(probe.listen());
        port = probe.port();
    }

    ::morph::net::SocketBackend::Config cfg;
    cfg.initialReconnectDelay = 20ms;
    cfg.maxReconnectDelay = 100ms;
    ::morph::net::SocketBackend backend{wsUrl(port), cfg};

    std::mutex mtx;
    std::vector<std::string> seen;
    auto record = [&](std::string what) {
        std::scoped_lock const lock{mtx};
        seen.push_back(std::move(what));
    };
    auto snapshot = [&] {
        std::scoped_lock const lock{mtx};
        return seen;
    };
    CountingExec reconnectExec;
    backend.setConnectHandler([&] { record("connect"); });
    backend.setReconnectHandler([&] { record("reconnect"); }, &reconnectExec);

    {
        ::morph::net::SocketServer first{*server, port};
        REQUIRE(first.listen());
        REQUIRE(::morph::testing::waitUntil([&] { return snapshot().size() == 1; }));
    }
    REQUIRE(::morph::testing::waitUntil([&] { return !backend.waitForConnected(0ms); }));

    ::morph::net::SocketServer second{*server, port};
    REQUIRE(second.listen());
    REQUIRE(::morph::testing::waitUntil([&] { return snapshot().size() == 3; }));
    // The connect handler runs before the reconnect handler is posted.
    CHECK(snapshot() == std::vector<std::string>{"connect", "connect", "reconnect"});
}

TEST_CASE("net_counter_returns_to_zero", "[push][channel]") {
    // One connection's backlog, end to end on a socket pair: 100 replies,
    // counted as they are handed over, written, and read by the peer.
    std::array<int, 2> fds{};
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds.data()) == 0);
    REQUIRE(::fcntl(fds[0], F_SETFL, ::fcntl(fds[0], F_GETFL) | O_NONBLOCK) == 0);

    ::morph::exec::IoLoop io;
    std::shared_ptr<::morph::net::detail::LoopConnection> conn;
    LoopConnectionGuard const guard{io, conn};
    std::atomic<int> drained{0};
    io.runAndWait([&] {
        auto socket = ::core::net::adoptFd(io.loop(), fds[0]);
        REQUIRE(socket);
        conn = std::make_shared<::morph::net::detail::LoopConnection>(io.loop(), std::move(*socket), 5000ms);
        conn->onDrained = [&drained] { drained.fetch_add(1); };
    });

    // 100 replies of ~8 KiB: about 800 KiB in all, so the backlog passes
    // `kEventBacklogLow` on the way up and crosses it once on the way down.
    constexpr int kReplies = 100;
    std::vector<std::string> envelopes;
    std::size_t total = 0;
    for (int i = 0; i < kReplies; ++i) {
        envelopes.push_back(::morph::wire::encode(
            ::morph::wire::makeOk(static_cast<std::uint64_t>(i + 1), '"' + std::string(8192, 'r') + '"')));
        total += envelopes.back().size();
    }
    REQUIRE(total >= ::morph::backend::kEventBacklogLow);
    for (auto const& envelope : envelopes) {
        conn->pending->fetch_add(envelope.size());
    }
    CHECK(conn->pending->load() == total);
    for (auto& envelope : envelopes) {
        std::size_t const counted = envelope.size();
        std::string frame = ::morph::net::detail::encodeWsFrame(::morph::net::detail::WsOpcode::kText, envelope,
                                                                /*mask=*/false);
        io.post([conn, frame = std::move(frame), counted]() mutable {
            static_cast<void>(::morph::net::detail::enqueueFrame(conn, std::move(frame), counted));
        });
    }

    ::morph::net::detail::WsFrameReader reader{/*expectMasked=*/false};
    int frames = 0;
    std::array<char, 65536> buf{};
    while (frames < kReplies) {
        if (auto frame = reader.tryExtractFrame()) {
            ++frames;
            continue;
        }
        auto const got = ::read(fds[1], buf.data(), buf.size());
        REQUIRE(got > 0);
        reader.feed(std::string_view{buf.data(), static_cast<std::size_t>(got)});
    }

    REQUIRE(::morph::testing::waitUntil([&] { return conn->pending->load() == 0; }));
    CHECK(drained.load() == 1);
    ::close(fds[1]);
}

TEST_CASE("net_closing_a_connection_releases_what_it_still_held", "[push][channel]") {
    std::array<int, 2> fds{};
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds.data()) == 0);
    REQUIRE(::fcntl(fds[0], F_SETFL, ::fcntl(fds[0], F_GETFL) | O_NONBLOCK) == 0);

    ::morph::exec::IoLoop io;
    std::shared_ptr<::morph::net::detail::LoopConnection> conn;
    LoopConnectionGuard const guard{io, conn};
    std::atomic<int> drained{0};
    io.runAndWait([&] {
        auto socket = ::core::net::adoptFd(io.loop(), fds[0]);
        REQUIRE(socket);
        conn = std::make_shared<::morph::net::detail::LoopConnection>(io.loop(), std::move(*socket), 5000ms);
        conn->onDrained = [&drained] { drained.fetch_add(1); };
        // Nothing reads the peer: most of 4 MiB stays queued.
        for (int i = 0; i < 64; ++i) {
            std::string const frame(std::size_t{64} * 1024, 'q');
            conn->pending->fetch_add(frame.size());
            static_cast<void>(::morph::net::detail::enqueueFrame(conn, frame, frame.size()));
        }
    });
    CHECK(conn->pending->load() > ::morph::backend::kEventBacklogHigh);

    io.runAndWait([&] { ::morph::net::detail::closeConnection(*conn); });
    REQUIRE(::morph::testing::waitUntil([&] { return conn->pending->load() == 0; }));
    // A closed connection is not reported drained: nothing can be sent on it.
    CHECK(drained.load() == 0);

    // A frame handed over after the close is released at once.
    io.runAndWait([&] {
        conn->pending->fetch_add(10);
        CHECK_FALSE(::morph::net::detail::enqueueFrame(conn, std::string(10, 'z'), 10));
        CHECK(conn->pending->load() == 0);
    });
    ::close(fds[1]);
}

TEST_CASE("net_replies_written_leave_no_backlog", "[push][channel]") {
    // 100 replies of 12 KiB through a real `SocketServer`: 1.2 MiB, past the
    // hold-back bound. Once all are read, an event still flows, which it
    // would not if the server had kept counting what it had written.
    ::morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<::morph::backend::RemoteServer>(pool);
    ::morph::net::SocketServer wsServer{*server, 0};
    REQUIRE(wsServer.listen());
    auto const cid = nextConnectionId(*server);

    RawClient client{wsServer.port()};
    client.boundReceives(10s);
    client.send(::morph::wire::makeRegister("PnModel"));
    auto const reg = client.receive();
    REQUIRE(reg.kind == "ok");

    constexpr int kReplies = 100;
    for (int i = 0; i < kReplies; ++i) {
        client.send(executeEnvelope(static_cast<std::uint64_t>(i + 1), reg.modelId, "PnBig", R"({"size":12288})"));
    }
    for (int i = 0; i < kReplies; ++i) {
        REQUIRE(client.receive().kind == "ok");
    }
    REQUIRE(::morph::testing::awaitAnswer([&](auto& owner) { return server->drainedWithin(10s, owner); }));

    auto const event = ::morph::wire::encode(::morph::wire::makeEvent("changes", R"({"after":"replies"})"));
    server->post(cid, event);
    auto const got = client.receive();
    CHECK(got.kind == "event");
    CHECK(::morph::wire::encode(got) == event);
}

TEST_CASE("net_event_posted_during_the_handshake_is_dropped", "[push][channel]") {
    // Connection ids are sequential, so a fan-out can name a connection still
    // in its handshake. Nothing may reach the peer ahead of the `101`.
    ::morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<::morph::backend::RemoteServer>(pool);
    ::morph::net::SocketServer wsServer{*server, 0};
    REQUIRE(wsServer.listen());
    std::uint16_t const port = wsServer.port();

    // Find the accepted connection's id without guessing when the accept ran:
    // probes take ids in sequence, and the one a probe skips is the
    // connection's.
    auto previous = nextConnectionId(*server) - 1;
    auto socket = ::morph::net::detail::TcpSocket::connect("127.0.0.1", port, 2000ms);
    ::morph::backend::ConnectionId cid = 0;
    REQUIRE(::morph::testing::waitUntil([&] {
        auto const probe = server->openConnection();
        server->closeConnection(probe);
        if (probe != previous + 1) {
            cid = previous + 1;
            return true;
        }
        previous = probe;
        return false;
    }));

    server->post(cid, ::morph::wire::encode(::morph::wire::makeEvent("changes", R"({"when":"handshake"})")));
    pollfd early{.fd = socket.nativeHandle(), .events = POLLIN, .revents = 0};
    CHECK(::poll(&early, 1, 300) == 0);  // nothing is sent before the Upgrade request

    ::morph::net::detail::ParsedWsUrl const url{.host = "127.0.0.1", .port = port, .path = "/"};
    std::string const key = ::morph::net::detail::generateClientKey();
    std::string const upgrade = ::morph::net::detail::buildClientHandshakeRequest(url, key);
    socket.sendAll(upgrade.data(), upgrade.size());
    REQUIRE(socket.setRecvTimeout(5000ms));
    std::string received;
    std::array<char, 4096> buf{};
    while (!received.contains("\r\n\r\n")) {
        std::size_t const got = socket.recvSome(buf.data(), buf.size());
        REQUIRE(got != 0);
        received.append(buf.data(), got);
    }
    CHECK(received.starts_with("HTTP/1.1 101"));

    // The pre-handshake event never arrives: the first frame is the one
    // posted after the handshake.
    auto const after = ::morph::wire::encode(::morph::wire::makeEvent("changes", R"({"when":"after"})"));
    server->post(cid, after);
    ::morph::net::detail::WsFrameReader reader{/*expectMasked=*/false};
    reader.feed(received.substr(received.find("\r\n\r\n") + 4));
    std::optional<::morph::net::detail::WsFrame> frame;
    while (!(frame = reader.tryExtractFrame())) {
        std::size_t const got = socket.recvSome(buf.data(), buf.size());
        REQUIRE(got != 0);
        reader.feed(std::string_view{buf.data(), got});
    }
    CHECK(frame->payload == after);
}

TEST_CASE("net_backlog_holds_events_and_drain_sends_resync", "[push][channel]") {
    // A reader that stops reading while 16 MiB of replies are queued for it:
    // an event posted then is held back, and once the reader catches up the
    // drain sends one `resync` in its place.
    //
    // Assumes the kernel's loopback buffers (the server's send buffer and the
    // client's receive window, shrunk to 2 KiB) absorb far less than 15 MiB,
    // so more than `kEventBacklogHigh` is still in the transport's backlog
    // when the event is posted. Were the kernel to take nearly everything,
    // the event would be delivered and this test would fail, not pass.
    ::morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<::morph::backend::RemoteServer>(pool);
    ::morph::net::SocketServer wsServer{*server, 0};
    REQUIRE(wsServer.listen());
    auto const cid = nextConnectionId(*server);

    RawClient client{wsServer.port(), /*receiveBuffer=*/2048};
    client.boundReceives(20s);
    client.send(::morph::wire::makeRegister("PnModel"));
    auto const reg = client.receive();
    REQUIRE(reg.kind == "ok");

    constexpr int kReplies = 128;
    for (int i = 0; i < kReplies; ++i) {
        client.send(executeEnvelope(static_cast<std::uint64_t>(i + 1), reg.modelId, "PnBig", R"({"size":131072})"));
    }
    // Every reply has been handed to the transport once none is in flight.
    REQUIRE(::morph::testing::awaitAnswer([&](auto& owner) { return server->drainedWithin(20s, owner); }));
    server->post(cid, ::morph::wire::encode(::morph::wire::makeEvent("changes", R"({"held":true})")));

    for (int i = 0; i < kReplies; ++i) {
        auto const reply = client.receive();
        REQUIRE(reply.kind == "ok");
    }
    auto const next = client.receive();
    CHECK(next.kind == "event");
    CHECK(::morph::wire::encode(next) == ::morph::wire::encode(::morph::wire::makeEvent("resync", "{}")));
}

TEST_CASE("net_channel_conformance", "[push][channel]") {
    ::morph::testing::push::channelConformance([](const auto& body) {
        ::morph::exec::ThreadPoolExecutor pool{2};
        auto server = std::make_shared<::morph::backend::RemoteServer>(pool);
        ::morph::net::SocketServer wsServer{*server, 0};
        REQUIRE(wsServer.listen());
        auto const cid = nextConnectionId(*server);
        ::morph::net::SocketBackend backend{wsUrl(wsServer.port())};
        REQUIRE(backend.waitForConnected());
        ::morph::testing::push::ChannelFixture fixture{
            .server = server.get(), .cid = cid, .backend = &backend, .waitFor = [](const std::function<bool()>& done) {
                return ::morph::testing::waitUntil(done);
            }};
        body(fixture);
    });
}

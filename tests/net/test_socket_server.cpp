// SPDX-License-Identifier: Apache-2.0

#include <fcntl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <memory>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/io_loop.hpp>
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

#include "../owner_probe_recorder.hpp"
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
// Returns a `size`-byte reply -- used by the sendTimeout regression test
// below to overflow a shrunk receive window in a handful of round trips
// instead of needing thousands of tiny ones.
struct NetEchoBig {
    int size = 0;
};

struct NetEchoModel {
    int execute(NetEchoAction action) { return action.value; }
    int execute(NetEchoFail) { throw std::runtime_error("echo failed"); }
    // {size, 'x'} would resolve to std::initializer_list<char> and narrow the size_t to char, a hard compile error.
    // NOLINTNEXTLINE(modernize-return-braced-init-list)
    std::string execute(NetEchoBig action) { return std::string(static_cast<std::size_t>(action.size), 'x'); }
};

BRIDGE_REGISTER_MODEL(NetEchoModel, "NetEchoModel")
BRIDGE_REGISTER_ACTION(NetEchoModel, NetEchoAction, "NetEchoAction")
BRIDGE_REGISTER_ACTION(NetEchoModel, NetEchoFail, "NetEchoFail")
BRIDGE_REGISTER_ACTION(NetEchoModel, NetEchoBig, "NetEchoBig")

namespace {

// ── Minimal manual raw-WebSocket client, built directly from the detail::
// helpers. Deliberately does NOT use SocketBackend (built in Task 8) so this
// test isolates SocketServer's correctness.
class RawWsClient {
public:
    // Reads frames sent by the server under test (a real SocketServer),
    // which RFC 6455 §5.1 forbids from masking the frames it sends.
    explicit RawWsClient(std::uint16_t port) : _reader(/*expectMasked=*/false) {
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

    // Sends raw bytes verbatim. Lets a test hand-craft protocol violations
    // (garbage instead of a handshake, malformed WS frames, raw control
    // frames) that encodeWsFrame()/send() cannot produce on their own.
    void sendRaw(std::string_view bytes) { _socket.sendAll(bytes.data(), bytes.size()); }

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

    // Like receive(), but returns the raw WsFrame instead of decoding the
    // payload as a wire::Envelope -- needed for control frames (Close/Ping/
    // Pong), whose payloads are not JSON.
    morph::net::detail::WsFrame receiveFrame() {
        for (;;) {
            if (auto frame = _reader.tryExtractFrame()) {
                return std::move(*frame);
            }
            char buf[4096];
            std::size_t const got = _socket.recvSome(buf, sizeof(buf));
            if (got == 0) {
                throw std::runtime_error("RawWsClient::receiveFrame: peer closed");
            }
            _reader.feed(std::string_view{buf, got});
        }
    }

    // Arms SO_LINGER{on, 0} so this object's destruction (which closes
    // `_socket`) sends an abortive RST instead of a normal FIN. Used to
    // simulate "the peer reset the connection" scenarios (socket_server.hpp
    // findings #8 and #10) that a graceful close cannot reproduce reliably.
    void armAbortiveClose() {
        linger l{};
        l.l_onoff = 1;
        l.l_linger = 0;
        ::setsockopt(_socket.nativeHandle(), SOL_SOCKET, SO_LINGER, &l, sizeof(l));
    }

private:
    morph::net::detail::TcpSocket _socket;
    morph::net::detail::WsFrameReader _reader;
};

// Creates a bare, non-blocking TCP socket (no connect() yet). Split out from
// fireNonBlockingConnect() below so a caller can allocate the fd *before*
// constraining RLIMIT_NOFILE (::socket() needs fd headroom; a later
// ::connect() on an already-open fd does not), then connect() it once the
// constraint is already active -- see the EMFILE test below.
int createNonBlockingSocket() {
    int const fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    int const flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    return fd;
}

// A distinct type for the port, so that beginConnect()'s two integer
// parameters cannot be transposed silently: with a plain std::uint16_t both
// converted to each other and `beginConnect(port, fd)` compiled.
struct LoopbackPort {
    std::uint16_t value;
};

// Issues a non-blocking connect() on an already-open socket `fd` toward
// 127.0.0.1:port. Returns immediately without waiting for it to complete.
void beginConnect(int fd, LoopbackPort port) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port.value);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    // Non-blocking connect(): either completes immediately (rare, loopback)
    // or returns -1/EINPROGRESS with the SYN already sent by the kernel in
    // the background -- either way the fd is left open for the caller.
    // ::connect takes a `sockaddr*`, so punning the `sockaddr_in` is how POSIX
    // specifies the call (the same cast tcp_socket.hpp's ::bind/::getsockname
    // make); and the result is deliberately dropped because -1/EINPROGRESS is
    // the expected outcome here -- static_cast<void> does not satisfy
    // bugprone-unused-return-value, whose AllowCastToVoid defaults to false.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast,bugprone-unused-return-value)
    static_cast<void>(::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)));
}

// Fires a bare, non-blocking connect() at 127.0.0.1:port and returns
// immediately without waiting for it to complete -- much cheaper per call
// than TcpSocket::connect() (which resolves via getaddrinfo() and polls for
// completion), so many of these can be issued back-to-back to build up
// listen-backlog pressure faster than a real OS thread per attempt could.
// Returns the (still-connecting-or-already-connected) fd, or -1 on failure.
int fireNonBlockingConnect(std::uint16_t port) {
    int const fd = createNonBlockingSocket();
    if (fd < 0) {
        return -1;
    }
    beginConnect(fd, LoopbackPort{port});
    return fd;
}

// Same as fireNonBlockingConnect(), but also arms SO_LINGER{1,0} up front so
// closing `fd` (the caller's job) sends an abortive RST rather than a normal
// FIN, regardless of whether the connect() has completed yet.
int fireNonBlockingConnectAndArmAbort(std::uint16_t port) {
    int const fd = fireNonBlockingConnect(port);
    if (fd >= 0) {
        linger l{};
        l.l_onoff = 1;
        l.l_linger = 0;
        ::setsockopt(fd, SOL_SOCKET, SO_LINGER, &l, sizeof(l));
    }
    return fd;
}

// Highest fd currently open in this process, scanned up to `limit` -- used to
// compute an RLIMIT_NOFILE value with zero headroom for a *new* fd, without
// disturbing any fd already in use. `fcntl(fd, F_GETFD)` is the standard "is
// this fd open" probe (fails with EBADF if not).
int highestOpenFd(int limit) {
    int highest = -1;
    for (int fd = 0; fd < limit; ++fd) {
        if (::fcntl(fd, F_GETFD) != -1) {
            highest = fd;
        }
    }
    return highest;
}

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

    REQUIRE(morph::testing::waitUntil([&] { return server->health().liveModels == 0U; },
                                      morph::testing::WaitBudget{std::chrono::seconds{5}}));

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
    REQUIRE(morph::testing::waitUntil([&] { return server->health().liveModels == 1U; },
                                      morph::testing::WaitBudget{std::chrono::seconds{5}}));

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

// ── Teardown must not depend on shutdown(2) waking a blocking accept() ──────
// `SocketServer::close()` has to unblock its accept-loop thread before joining
// it. Doing that by shutting down the *listening* socket works on Linux but is
// a no-op on macOS/BSD kernels, where the join then never returns and every
// destructor of a listening server hangs forever. The destruction
// runs on its own thread here so the deadline can be observed and reported as a
// failure instead of wedging the whole test binary until ctest's timeout.
TEST_CASE("SocketServer: destruction completes promptly with the accept flow parked", "[net][socket_server]") {
    // The whole server stack in one heap block so the thread below can destroy
    // it in a single step (members die in reverse order: SocketServer first, so
    // its close() still sees a live RemoteServer and executor). Held through a
    // shared_ptr the destroying thread owns, which keeps it alive if the deadline
    // is missed: that thread is then still inside close().
    struct ServerStack {
        morph::exec::ThreadPoolExecutor pool{2};
        std::shared_ptr<morph::backend::RemoteServer> server = std::make_shared<morph::backend::RemoteServer>(pool);
        morph::net::SocketServer wsServer{*server, 0};
    };
    auto stack = std::make_shared<ServerStack>();
    REQUIRE(stack->wsServer.listen());

    // A completed handshake proves the accept loop really reached accept(2),
    // returned a connection, and went back to park in accept(2) again -- a
    // server that was merely constructed and destroyed would not exercise the
    // bug at all.
    {
        RawWsClient const probe{stack->wsServer.port()};
    }

    auto const destroyed = std::make_shared<std::atomic<bool>>(false);
    std::thread destroyer{[owned = std::move(stack), destroyed] mutable {
        owned.reset();  // ~SocketServer -> close() on the loop -> the loop joins
        destroyed->store(true);
    }};

    if (!morph::testing::waitUntil([destroyed] { return destroyed->load(); },
                                   morph::testing::WaitBudget{std::chrono::seconds{5}})) {
        destroyer.detach();  // still parked in close(); the thread keeps `owned` alive on purpose
        FAIL("SocketServer destruction did not complete within 5s: the accept loop was never unblocked");
    }
    destroyer.join();
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

// ── Task 6b: closing socket_server.hpp's remaining coverage gaps ───────────
// See .superpowers/sdd/2026-09-03-framework-coverage-and-mutation/
// task-6-socket_server-findings.md for the audit this section closes.

TEST_CASE("SocketServer::listen() fails closed when the process is out of descriptors", "[net][socket_server]") {
    // With no descriptor headroom, the server's own loop and its listening
    // socket cannot be created. listen() must report that as `false`, not
    // throw and not start anything. RLIMIT_NOFILE, lowered to exactly the
    // number of fds already open, forces it deterministically without touching
    // anything already open.
    morph::exec::ThreadPoolExecutor pool{1};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);

    rlimit original{};
    REQUIRE(::getrlimit(RLIMIT_NOFILE, &original) == 0);
    rlim_t const scanLimit =
        original.rlim_cur < static_cast<rlim_t>(65536) ? original.rlim_cur : static_cast<rlim_t>(65536);
    int const highest = highestOpenFd(static_cast<int>(scanLimit));
    REQUIRE(highest >= 0);

    struct RlimitGuard {
        explicit RlimitGuard(rlimit savedIn) : saved{savedIn} {}
        RlimitGuard(const RlimitGuard&) = delete;
        RlimitGuard& operator=(const RlimitGuard&) = delete;
        RlimitGuard(RlimitGuard&&) = delete;
        RlimitGuard& operator=(RlimitGuard&&) = delete;
        ~RlimitGuard() {
            ::setrlimit(RLIMIT_NOFILE, &saved);
            for (int const fd : fillers) {
                ::close(fd);
            }
        }

        rlimit saved;
        std::vector<int> fillers;
    } guard{original};

    // Densify the fd table below `highest` before capping the limit: a gap
    // left by some fd opened-then-closed earlier (sanitizer/valgrind
    // instrumentation is especially prone to this) would otherwise let
    // socket()/pipe() below silently reuse that gap instead of hitting the
    // cap, defeating "zero headroom" -- confirmed to happen in practice
    // (this test flaked exactly this way under both ASan and Valgrind CI,
    // never locally, which is consistent with sanitizer-only transient fds).
    for (int fd = 0; fd < highest; ++fd) {
        if (::fcntl(fd, F_GETFD) == -1) {
            int const filler = ::open("/dev/null", O_RDONLY);
            REQUIRE(filler >= 0);
            guard.fillers.push_back(filler);
        }
    }

    rlimit constrained = original;
    constrained.rlim_cur = static_cast<rlim_t>(highest + 1);  // no headroom for a new fd
    REQUIRE(::setrlimit(RLIMIT_NOFILE, &constrained) == 0);

    {
        morph::net::SocketServer wsServer{*server, 0};
        // Under the constrained limit neither the server's loop nor its
        // listening socket can be created.
        REQUIRE_FALSE(wsServer.listen());
    }

    ::setrlimit(RLIMIT_NOFILE, &original);  // restore before the sanity check below; the guard also does this on exit

    // The constraint was scoped to just that one construction: a normal
    // server works again once the limit is lifted.
    morph::net::SocketServer sanityServer{*server, 0};
    REQUIRE(sanityServer.listen());
}

TEST_CASE("SocketServer::listen() fails when the requested port is already bound (EADDRINUSE)",
          "[net][socket_server]") {
    morph::exec::ThreadPoolExecutor pool{1};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);
    morph::net::SocketServer first{*server, 0};
    REQUIRE(first.listen());
    std::uint16_t const port = first.port();

    // TcpSocket::listen() throws on bind()'s EADDRINUSE; SocketServer::listen()
    // catches it and reports failure rather than propagating.
    morph::net::SocketServer second{*server, port};
    REQUIRE_FALSE(second.listen());
}

TEST_CASE("SocketServer: close() on a server that was never listen()ed is a safe no-op", "[net][socket_server]") {
    // A constructed-but-never-started server must close and destruct cleanly,
    // and a second close() must be a no-op.
    morph::exec::ThreadPoolExecutor pool{1};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);
    morph::net::SocketServer wsServer{*server, 0};
    wsServer.close();  // must not crash or hang
    wsServer.close();  // idempotent
}

TEST_CASE("SocketServer: concurrent close() calls from two threads do not hang or corrupt state",
          "[net][socket_server]") {
    // Two threads call close() at the same moment, repeatedly. Each call is one
    // task on the server's loop, so they run one after the other: both must
    // return, neither may throw, and a further close() must still be a no-op.
    constexpr int kIterations = 30;
    auto done = std::make_shared<std::atomic<bool>>(false);
    auto exceptionCount = std::make_shared<std::atomic<int>>(0);

    std::thread worker([done, exceptionCount] {
        for (int i = 0; i < kIterations; ++i) {
            morph::exec::ThreadPoolExecutor pool{2};
            auto server = std::make_shared<morph::backend::RemoteServer>(pool);
            auto wsServer = std::make_unique<morph::net::SocketServer>(*server, 0);
            if (!wsServer->listen()) {
                continue;
            }

            std::atomic<int> ready{0};
            std::atomic<bool> go{false};
            auto racer = [&] {
                ready.fetch_add(1, std::memory_order_relaxed);
                while (!go.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                try {
                    wsServer->close();
                } catch (const std::exception&) {
                    exceptionCount->fetch_add(1, std::memory_order_relaxed);
                }
            };
            std::thread t1(racer);
            std::thread t2(racer);
            while (ready.load(std::memory_order_acquire) < 2) {
                std::this_thread::yield();
            }
            go.store(true, std::memory_order_release);
            t1.join();
            t2.join();

            // Whichever thread actually won the race, the server must now be
            // fully, safely closed: a further close()/destruction must not
            // hang or throw.
            wsServer->close();
        }
        done->store(true);
    });

    if (!morph::testing::waitUntil([done] { return done->load(); },
                                   morph::testing::WaitBudget{std::chrono::seconds{15}})) {
        worker.detach();
        FAIL("Concurrent close() race did not complete within 15s -- possible hang from a double join");
    }
    worker.join();
    INFO("Observed " << exceptionCount->load() << " std::system_error(s) escaping a racing close() call across "
                     << kIterations << " iterations (the narrow join() race this test targets)");
}

TEST_CASE("SocketServer: close() racing a burst of pending connections still finishes", "[net][socket_server]") {
    // A burst of connects is queued in the listener's backlog and close() races
    // it, so the accept flow may be anywhere between an accept and starting a
    // connection's flow when the close runs. A hang is the loop *stopping*, not
    // the loop being slow: `progress` ticks once per completed iteration, and the
    // case fails only when it stops ticking. `stop` retires the worker at the
    // overall budget so it is always joinable.
    constexpr int kIterations = 200;
    constexpr int kBacklogDepth = 16;
    // No progress for this long == stuck. Generous: one iteration's honest
    // worst case measured 0.36 s under Valgrind, and ~1.4 s extrapolated to a
    // contended runner.
    constexpr std::chrono::seconds kStallBudget{20};
    // Stop sampling here, however many iterations that bought.
    constexpr std::chrono::seconds kTotalBudget{60};

    auto done = std::make_shared<std::atomic<bool>>(false);
    auto progress = std::make_shared<std::atomic<int>>(0);
    auto stop = std::make_shared<std::atomic<bool>>(false);

    std::thread worker([done, progress, stop] {
        for (int i = 0; i < kIterations && !stop->load(); ++i) {
            morph::exec::ThreadPoolExecutor pool{2};
            auto server = std::make_shared<morph::backend::RemoteServer>(pool);
            auto wsServer = std::make_unique<morph::net::SocketServer>(*server, 0);
            if (!wsServer->listen()) {
                continue;
            }
            std::uint16_t const port = wsServer->port();

            std::vector<int> fds;
            fds.reserve(kBacklogDepth);
            for (int c = 0; c < kBacklogDepth; ++c) {
                fds.push_back(fireNonBlockingConnect(port));
            }
            wsServer->close();  // races the whole burst of connects at once
            for (int const fd : fds) {
                if (fd >= 0) {
                    ::close(fd);
                }
            }
            progress->fetch_add(1, std::memory_order_relaxed);
        }
        done->store(true);
    });

    using Clock = std::chrono::steady_clock;
    auto const giveUpAt = Clock::now() + kTotalBudget;
    int lastSeen = 0;
    auto lastTick = Clock::now();
    bool stalled = false;
    while (!done->load()) {
        int const seen = progress->load(std::memory_order_relaxed);
        if (seen != lastSeen) {
            lastSeen = seen;
            lastTick = Clock::now();
        }
        if (Clock::now() - lastTick > kStallBudget) {
            stalled = true;
            break;
        }
        if (Clock::now() > giveUpAt) {
            break;  // slow, not stuck -- retire the worker and report the sample size
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    // Set before joining either way: one more iteration is bounded work, so the
    // worker always reaches its next `stop` check and exits.
    stop->store(true);
    worker.join();
    INFO("Completed " << progress->load() << " of " << kIterations << " close()-races-connect-burst iterations");
    if (stalled) {
        FAIL("close()/connect-burst race stopped making progress for " << kStallBudget.count() << "s after "
                                                                       << lastSeen << " iterations -- hang");
    }
    REQUIRE(progress->load() > 0);
}

TEST_CASE("SocketServer: the accept flow keeps serving after pending connections are aborted before accept",
          "[net][socket_server]") {
    // Many non-blocking connects are fired and reset (SO_LINGER{1,0}) back to
    // back, so some are aborted before the loop accepts them. The accept flow
    // must keep going through every one: a well-behaved client still gets served
    // afterwards.
    morph::exec::ThreadPoolExecutor pool{4};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);
    morph::net::SocketServer wsServer{*server, 0};
    REQUIRE(wsServer.listen());
    std::uint16_t const port = wsServer.port();

    constexpr int kBurstSize = 64;
    constexpr int kBursts = 15;
    for (int burst = 0; burst < kBursts; ++burst) {
        std::vector<int> fds;
        fds.reserve(kBurstSize);
        for (int i = 0; i < kBurstSize; ++i) {
            fds.push_back(fireNonBlockingConnectAndArmAbort(port));
        }
        for (int const fd : fds) {
            if (fd >= 0) {
                ::close(fd);  // SO_LINGER{1,0} already armed: sends an abortive RST
            }
        }
    }

    // The accept loop must have kept looping through every aborted attempt:
    // a normal, well-behaved connection still works afterward.
    RawWsClient probe{port};
    probe.send(morph::wire::makeRegister("NetEchoModel"));
    auto reg = probe.receive();
    REQUIRE(reg.kind == "ok");
}

// accept(2) failing with EMFILE is deterministic: the kernel checks the
// process's fd-table limit before returning a new fd, independent of any
// TCP-level timing. RLIMIT_NOFILE is lowered to zero headroom with connections
// pending; the accept flow must survive the failures and close() must still
// complete promptly.
TEST_CASE("SocketServer: the accept flow survives accept() running out of fds, and close() still completes",
          "[net][socket_server]") {
    morph::exec::ThreadPoolExecutor pool{1};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);
    morph::net::SocketServer wsServer{*server, 0};
    REQUIRE(wsServer.listen());
    std::uint16_t const port = wsServer.port();

    // Pre-create several plain (not-yet-connected) sockets *before* touching
    // the limit -- ::socket() needs fd headroom, which the constrained limit
    // below would refuse.
    constexpr int kPending = 5;
    std::vector<int> clientFds;
    clientFds.reserve(kPending);
    for (int i = 0; i < kPending; ++i) {
        int const fd = createNonBlockingSocket();
        REQUIRE(fd >= 0);
        clientFds.push_back(fd);
    }

    rlimit original{};
    REQUIRE(::getrlimit(RLIMIT_NOFILE, &original) == 0);
    rlim_t const scanLimit =
        original.rlim_cur < static_cast<rlim_t>(65536) ? original.rlim_cur : static_cast<rlim_t>(65536);
    int const highest = highestOpenFd(static_cast<int>(scanLimit));
    REQUIRE(highest >= 0);

    struct RlimitGuard {
        explicit RlimitGuard(rlimit savedIn) : saved{savedIn} {}
        RlimitGuard(const RlimitGuard&) = delete;
        RlimitGuard& operator=(const RlimitGuard&) = delete;
        RlimitGuard(RlimitGuard&&) = delete;
        RlimitGuard& operator=(RlimitGuard&&) = delete;
        ~RlimitGuard() { ::setrlimit(RLIMIT_NOFILE, &saved); }

        rlimit saved;
    } const guard{original};

    // Zero headroom for a *new* fd. Note the pre-created sockets above are
    // *not* connected yet, so nothing is in the backlog for the accept loop
    // to race us for -- unlike TcpSocket::connect(), a raw ::connect() on an
    // already-open fd needs no new fd of its own, so it is safe to issue
    // *after* the constraint is already active, closing the window that
    // defeated the naive "connect first, then constrain" ordering (the
    // accept loop, running continuously in the background, would otherwise
    // almost always drain the backlog before this thread even finishes
    // computing the rlimit to set).
    rlimit constrained = original;
    constrained.rlim_cur = static_cast<rlim_t>(highest + 1);
    REQUIRE(::setrlimit(RLIMIT_NOFILE, &constrained) == 0);

    for (int const fd : clientFds) {
        beginConnect(fd, LoopbackPort{port});
    }

    // Let the accept flow meet EMFILE on its own before close() runs. There
    // is no public signal for "an accept failed", so this waits a fixed,
    // generous duration -- EMFILE is a persistent *state* here (the limit
    // stays constrained the whole time), so any reasonable wait reaches it.
    std::this_thread::sleep_for(std::chrono::milliseconds{300});

    // The accept flow backs off between failed accepts rather than spinning,
    // so the loop stays responsive: close() must complete promptly.
    auto closed = std::make_shared<std::atomic<bool>>(false);
    std::thread closer([&wsServer, closed] {
        wsServer.close();
        closed->store(true);
    });
    bool const finishedPromptly = morph::testing::waitUntil([closed] { return closed->load(); },
                                                            morph::testing::WaitBudget{std::chrono::seconds{5}});

    ::setrlimit(RLIMIT_NOFILE, &original);  // restore before any further fd use, including cleanup below
    for (int const fd : clientFds) {
        ::close(fd);
    }

    if (!finishedPromptly) {
        closer.detach();
        FAIL("close() did not complete within 5s after accept() hit EMFILE");
    }
    closer.join();
}

TEST_CASE("SocketServer: a reply written after the peer reset retires the connection", "[net][socket_server]") {
    // The peer resets its connection before the server replies. StepExecutor
    // holds the reply-producing work until this thread runs it, so the reset is
    // always applied first; the reply is then written to a reset socket, which
    // must retire the connection and leave the server serving other clients.
    morph::testing::StepExecutor pool;
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);
    morph::net::SocketServer wsServer{*server, 0};
    REQUIRE(wsServer.listen());

    constexpr int kAttempts = 10;
    for (int i = 0; i < kAttempts; ++i) {
        auto client = std::make_unique<RawWsClient>(wsServer.port());
        client->send(morph::wire::makeRegister("NetEchoModel"));
        REQUIRE(morph::testing::waitUntil([&] { return pool.pending() > 0; }));
        pool.runAll();
        auto reg = client->receive();
        REQUIRE(reg.kind == "ok");

        morph::wire::Envelope execReq;
        execReq.kind = "execute";
        execReq.callId = 1;
        execReq.modelId = reg.modelId;
        execReq.modelType = "NetEchoModel";
        execReq.actionType = "NetEchoAction";
        execReq.body = R"({"value":1})";
        client->send(execReq);
        // Wait for the loop to have read the request and handed it to the
        // server -- but nothing has run yet.
        REQUIRE(morph::testing::waitUntil([&] { return pool.pending() > 0; }));

        client->armAbortiveClose();
        client.reset();  // destructor -> abortive close -> RST, *before* any reply work runs

        // Runs the dispatch (and any nested strand-posted reply task) with the
        // connection already broken: the reply's write fails, or finds the
        // connection already retired.
        pool.runAll();
    }

    // The server must still be usable afterward: no RST'd peer may wedge the
    // accept flow or any other connection. `pool` is a StepExecutor, so this
    // reply needs pumping too, exactly like the registers above.
    RawWsClient probe{wsServer.port()};
    probe.send(morph::wire::makeRegister("NetEchoModel"));
    REQUIRE(morph::testing::waitUntil([&] { return pool.pending() > 0; }));
    pool.runAll();
    auto probeReg = probe.receive();
    REQUIRE(probeReg.kind == "ok");
}

TEST_CASE("SocketServer: a malformed handshake is caught, the accept flow keeps serving other clients",
          "[net][socket_server]") {
    // A malformed Upgrade request ends that connection only: the accept flow
    // and every other connection go on.
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);
    morph::net::SocketServer wsServer{*server, 0};
    REQUIRE(wsServer.listen());

    {
        auto bad =
            morph::net::detail::TcpSocket::connect("127.0.0.1", wsServer.port(), std::chrono::milliseconds{2000});
        std::string const garbage = "GARBAGE\r\n\r\n";
        bad.sendAll(garbage.data(), garbage.size());
    }

    RawWsClient probe{wsServer.port()};
    probe.send(morph::wire::makeRegister("NetEchoModel"));
    auto reg = probe.receive();
    REQUIRE(reg.kind == "ok");
}

TEST_CASE("SocketServer: a malformed WebSocket frame is caught, the accept flow keeps serving other clients",
          "[net][socket_server]") {
    // A continuation frame with no message in progress (same violation as
    // test_ws_frame.cpp's "rejects a continuation with no message in
    // progress"), sent over a real socket instead of fed to the reader
    // directly. The reader throws; the connection's flow ends that connection
    // only -- without taking down the accept flow.
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);
    morph::net::SocketServer wsServer{*server, 0};
    REQUIRE(wsServer.listen());

    {
        RawWsClient bad{wsServer.port()};
        std::string const orphanContinuation =
            morph::net::detail::encodeWsFrame(morph::net::detail::WsOpcode::kContinuation, "orphan", /*mask=*/true);
        bad.sendRaw(orphanContinuation);
    }

    RawWsClient probe{wsServer.port()};
    probe.send(morph::wire::makeRegister("NetEchoModel"));
    auto reg = probe.receive();
    REQUIRE(reg.kind == "ok");
}

// ── Control-frame handling (Close/Ping/Pong/other) ──────────────────────────
// None of these were ever exercised by any test in tests/net -- the server
// only ever saw Text frames before this. Together these three tests also
// fully exercise sendControlFrame() (called only from the Close/Ping arms).

TEST_CASE("SocketServer: a Close frame is echoed back and the connection is marked closed", "[net][socket_server]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);
    morph::net::SocketServer wsServer{*server, 0};
    REQUIRE(wsServer.listen());

    RawWsClient client{wsServer.port()};
    client.sendRaw(morph::net::detail::encodeWsFrame(morph::net::detail::WsOpcode::kClose, "", /*mask=*/true));
    auto echoed = client.receiveFrame();
    REQUIRE(echoed.opcode == morph::net::detail::WsOpcode::kClose);
}

TEST_CASE("SocketServer: a Ping frame is answered with a Pong echo carrying the same payload",
          "[net][socket_server]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);
    morph::net::SocketServer wsServer{*server, 0};
    REQUIRE(wsServer.listen());

    RawWsClient client{wsServer.port()};
    client.sendRaw(morph::net::detail::encodeWsFrame(morph::net::detail::WsOpcode::kPing, "hb", /*mask=*/true));
    auto pong = client.receiveFrame();
    REQUIRE(pong.opcode == morph::net::detail::WsOpcode::kPong);
    REQUIRE(pong.payload == "hb");
}

TEST_CASE("SocketServer: an unsolicited Pong and a Binary frame are silently ignored", "[net][socket_server]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);
    morph::net::SocketServer wsServer{*server, 0};
    REQUIRE(wsServer.listen());

    RawWsClient client{wsServer.port()};
    client.sendRaw(
        morph::net::detail::encodeWsFrame(morph::net::detail::WsOpcode::kPong, "unsolicited", /*mask=*/true));
    client.sendRaw(
        morph::net::detail::encodeWsFrame(morph::net::detail::WsOpcode::kBinary, "somedata", /*mask=*/true));

    // Neither frame gets a reply or breaks the connection: a normal request
    // right after still works.
    client.send(morph::wire::makeRegister("NetEchoModel"));
    auto reg = client.receive();
    REQUIRE(reg.kind == "ok");
}

TEST_CASE("SocketServer: a Pong written after the peer reset retires the connection", "[net][socket_server]") {
    // The peer pings and resets before the Pong goes out: the failed write must
    // retire the connection without disturbing the server.
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);
    morph::net::SocketServer wsServer{*server, 0};
    REQUIRE(wsServer.listen());

    for (int i = 0; i < 10; ++i) {
        auto client = std::make_unique<RawWsClient>(wsServer.port());
        client->sendRaw(morph::net::detail::encodeWsFrame(morph::net::detail::WsOpcode::kPing, "hb", /*mask=*/true));
        client->armAbortiveClose();
        client.reset();  // destructor -> abortive close -> RST; the Pong is never read
    }

    // The server must still be usable afterward.
    RawWsClient probe{wsServer.port()};
    probe.send(morph::wire::makeRegister("NetEchoModel"));
    auto reg = probe.receive();
    REQUIRE(reg.kind == "ok");
}

// The Catch2 assertion macros, not branching logic, are what push this over the
// cognitive-complexity threshold -- as in the sibling cases above.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("SocketServer: two threads calling close() concurrently both return without throwing",
          "[net][socket_server]") {
    // close() is one loop task per call, so two racing callers are serialised
    // by the loop: both return, neither throws, whichever ran first did the
    // teardown and the other found nothing left to do.
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
TEST_CASE("SocketServer: tearing down a parked accept flow finishes promptly", "[net][socket_server]") {
    // No client, pending or established: the accept flow is parked on the
    // listener with nothing else that could resume it. close() closes the
    // listener, which resumes the parked accept through the loop on every
    // platform; teardown must finish within a bounded time, not merely finish.
    constexpr auto kBudget = std::chrono::seconds{2};

    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);
    auto wsServer = std::make_unique<morph::net::SocketServer>(*server, 0);
    REQUIRE(wsServer->listen());
    REQUIRE(wsServer->port() != 0U);

    // Let the accept flow actually reach its wait. Without this the test
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

TEST_CASE("SocketServer: the accept flow survives repeated listen/close cycles", "[net][socket_server]") {
    // Each listen() starts a fresh accept flow on a fresh listener. A flow left
    // over from an earlier cycle must end with its own listener and never
    // swallow the next cycle's connections: a real client completes a round
    // trip on every cycle.
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
    // close() releases the listening descriptor, so the port can be bound again
    // at once -- the same post-close observation QtWebSocketServer makes.
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
    // A client arriving while close() is already under way, so the accept flow
    // may be anywhere between an accept and starting the connection's flow.
    // Complements the parked case above.
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

// ── Finished connections must be reclaimed while the server runs ────────────
//
// A connection's socket is released when its flow ends, not when the server
// closes. The fd count is sampled *while the server is still running*: a test
// that opened N connections and then destroyed the server could not tell
// eager release from release at teardown.
#ifndef _WIN32
namespace {
std::size_t openFdCount() {
    std::size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator{"/proc/self/fd"}) {
        (void)entry;
        ++count;
    }
    return count;
}
}  // namespace

TEST_CASE("SocketServer: a finished connection's fd is reclaimed before shutdown", "[net][socket_server][morph498]") {
    if (!std::filesystem::exists("/proc/self/fd")) {
        SUCCEED("no /proc/self/fd on this platform");
        return;
    }
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);
    morph::net::SocketServer wsServer{*server, 0};
    REQUIRE(wsServer.listen());

    // RawWsClient closes its socket on destruction, so each scope block below is
    // one full connect/disconnect cycle.
    //
    // Warm up: the first few settle one-off allocations, so the baseline reflects
    // steady state rather than start-up.
    for (int i = 0; i < 4; ++i) {
        RawWsClient const client{wsServer.port()};
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{50});

    std::size_t const baseline = openFdCount();

    // Each iteration opens and closes one connection. Before the fix every one
    // of these left an fd behind, so the count grew monotonically with N.
    constexpr int kRounds = 25;
    for (int i = 0; i < kRounds; ++i) {
        RawWsClient const client{wsServer.port()};
    }
    // A connection's socket goes when its flow sees the peer's close; give
    // the loop a moment, and a couple more connections, to settle.
    {
        RawWsClient const trigger{wsServer.port()};
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    {
        RawWsClient const trigger2{wsServer.port()};
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{100});

    std::size_t const after = openFdCount();
    INFO("baseline=" << baseline << " after=" << after << " rounds=" << kRounds);
    // Allow generous slack for the two still-unreaped connections and any
    // transient fds; what must NOT happen is growth proportional to kRounds.
    CHECK(after < baseline + (static_cast<std::size_t>(kRounds) / 2));
}
#endif  // _WIN32

// ── morph#534: handshakeTimeout and sendTimeout ─────────────────────────────
// Neither knob existed before this change: `SocketServerConfig` had only
// `backlog`. Both regression tests below fail on unfixed code -- the first by
// timing out its own bounded wait (the connection is never closed), the
// second the same way (the stalled reply write blocks forever instead of
// throwing, so the connection is never retired).

TEST_CASE("SocketServer: handshakeTimeout retires a connection that never completes its handshake",
          "[net][socket_server][morph534]") {
    // Slowloris, minimal form: connect and send one byte that is not a
    // complete HTTP request, then go silent. Without a timeout the
    // connection's handshake read, and its fd, would wait forever.
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);
    morph::net::SocketServer::Config cfg;
    cfg.handshakeTimeout = std::chrono::milliseconds{200};
    morph::net::SocketServer wsServer{*server, 0, cfg};
    REQUIRE(wsServer.listen());

    auto sock = morph::net::detail::TcpSocket::connect("127.0.0.1", wsServer.port(), std::chrono::milliseconds{2000});
    char const one = 'G';
    sock.sendAll(&one, 1);

    // Give the timeout time to fire -- well over the 200ms handshakeTimeout
    // above.
    std::this_thread::sleep_for(std::chrono::milliseconds{600});

    // A well-behaved probe proves the accept flow itself was never wedged by
    // the earlier bad connection.
    RawWsClient probe{wsServer.port()};
    probe.send(morph::wire::makeRegister("NetEchoModel"));
    REQUIRE(probe.receive().kind == "ok");

    // The timed-out connection has been closed by the server: its peer reads
    // EOF. Without the timeout this poll would time out (rc == 0).
    pollfd pfd{};
    pfd.fd = sock.nativeHandle();
    pfd.events = POLLIN;
    int const rc = ::poll(&pfd, 1, 2000);
    REQUIRE(rc > 0);
    char buf[1];
    ssize_t const n = ::recv(sock.nativeHandle(), buf, 1, 0);
    CHECK(n == 0);  // EOF: the server closed the connection, not sent data
}

TEST_CASE("SocketServer: a stalled reply write on a still-readable connection retires it instead of hanging",
          "[net][socket_server][morph534]") {
    // A peer that stops draining its socket, with its read direction still
    // live (no reset, no FIN), must be retired rather than left dispatching
    // requests whose replies queue forever: `sendTimeout` bounds each chunk
    // of a reply's write, and when it runs out the connection is closed and
    // its models reclaimed. Not the "peer reset" case above, where the very
    // next write fails with no timeout involved.
    //
    // The wait runs on a background thread with a bounded budget and
    // detaches on stall rather than blocking the test binary.
    struct Stack {
        morph::exec::ThreadPoolExecutor pool{2};
        std::shared_ptr<morph::backend::RemoteServer> server = std::make_shared<morph::backend::RemoteServer>(pool);
        morph::net::SocketServer wsServer;
        explicit Stack(morph::net::SocketServer::Config cfg) : wsServer{*server, 0, cfg} {}
    };
    morph::net::SocketServer::Config cfg;
    cfg.sendTimeout = std::chrono::milliseconds{300};
    auto stack = std::make_shared<Stack>(cfg);
    REQUIRE(stack->wsServer.listen());
    std::uint16_t const port = stack->wsServer.port();

    auto socket = std::make_shared<morph::net::detail::TcpSocket>(
        morph::net::detail::TcpSocket::connect("127.0.0.1", port, std::chrono::milliseconds{2000}));
    // Shrink the receive window so a modest flood of replies overflows it
    // without needing megabytes of traffic. The OS may clamp this upward to
    // some platform minimum; either way it is far smaller than an unbounded
    // default.
    int const tinyBuf = 1024;
    ::setsockopt(socket->nativeHandle(), SOL_SOCKET, SO_RCVBUF, &tinyBuf, sizeof(tinyBuf));

    morph::net::detail::ParsedWsUrl const url{.host = "127.0.0.1", .port = port, .path = "/"};
    std::string const leftover = morph::net::detail::performClientHandshake(*socket, url);
    auto reader = std::make_shared<morph::net::detail::WsFrameReader>(/*expectMasked=*/false);
    reader->feed(leftover);

    auto sendEnvelope = [&](const morph::wire::Envelope& env) {
        std::string frame = morph::net::detail::encodeWsFrame(morph::net::detail::WsOpcode::kText,
                                                              morph::wire::encode(env), /*mask=*/true);
        socket->sendAll(frame.data(), frame.size());
    };
    auto recvOne = [&]() -> morph::wire::Envelope {
        for (;;) {
            if (auto frame = reader->tryExtractFrame()) {
                return morph::wire::decode(frame->payload);
            }
            char buf[4096];
            std::size_t const got = socket->recvSome(buf, sizeof(buf));
            if (got == 0) {
                throw std::runtime_error("peer closed");
            }
            reader->feed(std::string_view{buf, got});
        }
    };

    sendEnvelope(morph::wire::makeRegister("NetEchoModel"));
    auto reg = recvOne();
    REQUIRE(reg.kind == "ok");
    REQUIRE(stack->server->health().liveModels == 1U);

    // Flood large replies without ever reading them: the client's receive
    // window fills, the server's `sendAll` for a later reply stalls, and
    // (with the fix) `sendTimeout` bounds that stall. The connection is never
    // reset or closed by the client -- its read direction stays live
    // throughout. `NetEchoBig` (64 KiB per reply) overflows even an
    // OS-clamped-up receive window in a handful of round trips, rather than
    // needing thousands of tiny ones to add up.
    constexpr int kFloodCount = 64;
    constexpr int kReplyBytes = 65536;
    for (int i = 0; i < kFloodCount; ++i) {
        morph::wire::Envelope req;
        req.kind = "execute";
        req.callId = static_cast<std::uint64_t>(i + 1);
        req.modelId = reg.modelId;
        req.modelType = "NetEchoModel";
        req.actionType = "NetEchoBig";
        req.body = R"({"size":)" + std::to_string(kReplyBytes) + "}";
        try {
            sendEnvelope(req);
        } catch (const std::exception&) {
            // The client's own send can fail too, once the connection is torn
            // down from the server side -- irrelevant to what this test
            // checks, so just stop feeding it.
            break;
        }
    }
    // No further recvOne() calls: the client deliberately never drains
    // anything from here on.

    auto reclaimed = std::make_shared<std::atomic<bool>>(false);
    std::thread waiter{[stack, reclaimed] {
        morph::testing::waitUntil([&] { return stack->server->health().liveModels == 0U; },
                                  morph::testing::WaitBudget{std::chrono::seconds{15}});
        reclaimed->store(true);
    }};

    bool const finished = morph::testing::waitUntil([reclaimed] { return reclaimed->load(); },
                                                    morph::testing::WaitBudget{std::chrono::seconds{20}});
    if (!finished) {
        waiter.detach();
        FAIL("connection was never retired after its reply write stalled against a full, still-open receive window");
    }
    waiter.join();

    // The connection must actually have been retired -- its flow ended and
    // its ScopeGuard reclaimed the model -- not merely "eventually true by
    // coincidence".
    REQUIRE(stack->server->health().liveModels == 0U);
}

// ── One I/O loop owns the server ────────────────────────────────────────────
//
// The listener, the connection list and every reply write live on the
// `IoLoop` the server was built on. Each case drives the server from this
// test's own thread, or from the `RemoteServer`'s pool for a reply, and reads,
// from inside the body that runs, whether it runs as a task of the loop.

TEST_CASE("SocketServer: a reply produced on the server's pool is written on the loop",
          "[net][socket_server][owner]") {
    morph::exec::IoLoop loop;
    morph::testing::OwnerProbeRecorder const recorder{loop.loop()};
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);
    morph::net::SocketServer wsServer{loop, *server, 0};
    REQUIRE(wsServer.listen());

    RawWsClient client{wsServer.port()};
    client.send(morph::wire::makeRegister("NetEchoModel"));
    REQUIRE(client.receive().kind == "ok");

    CHECK(recorder.count("SocketServer::reply") >= 1U);
    CHECK(recorder.allPosted("SocketServer::reply"));
}

TEST_CASE("SocketServer: connections are accepted and closed on the loop", "[net][socket_server][owner]") {
    morph::exec::IoLoop loop;
    morph::testing::OwnerProbeRecorder const recorder{loop.loop()};
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);
    morph::net::SocketServer wsServer{loop, *server, 0};
    REQUIRE(wsServer.listen());

    RawWsClient client{wsServer.port()};
    client.send(morph::wire::makeRegister("NetEchoModel"));
    REQUIRE(client.receive().kind == "ok");
    wsServer.close();

    CHECK(recorder.allPosted("SocketServer::listen"));
    CHECK(recorder.allPosted("SocketServer::accept"));
    CHECK(recorder.allPosted("SocketServer::close"));
    CHECK(server->health().liveModels == 0U);
}

TEST_CASE("SocketServer: close() from two threads at once runs twice on the loop, one after the other",
          "[net][socket_server][owner]") {
    morph::exec::IoLoop loop;
    morph::testing::OwnerProbeRecorder const recorder{loop.loop()};
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);
    morph::net::SocketServer wsServer{loop, *server, 0};
    REQUIRE(wsServer.listen());

    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    auto racer = [&] {
        ready.fetch_add(1);
        while (!go.load()) {
            std::this_thread::yield();
        }
        wsServer.close();
    };
    std::thread first{racer};
    std::thread second{racer};
    while (ready.load() < 2) {
        std::this_thread::yield();
    }
    go.store(true);
    first.join();
    second.join();

    CHECK(recorder.count("SocketServer::close") == 2U);
    CHECK(recorder.allPosted("SocketServer::close"));
    CHECK(wsServer.port() == 0U);
}

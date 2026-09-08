// SPDX-License-Identifier: Apache-2.0

#include <fcntl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

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

// ── Teardown must not depend on shutdown(2) waking a blocking accept() ──────
// `SocketServer::close()` has to unblock its accept-loop thread before joining
// it. Doing that by shutting down the *listening* socket works on Linux but is
// a no-op on macOS/BSD kernels, where the join then never returns and every
// destructor of a listening server hangs forever (morph#437). The destruction
// runs on its own thread here so the deadline can be observed and reported as a
// failure instead of wedging the whole test binary until ctest's timeout.
TEST_CASE("SocketServer: destruction completes promptly with the accept loop parked in accept()",
          "[net][socket_server]") {
    // The whole server stack in one heap block so the thread below can destroy
    // it in a single step (members die in reverse order: SocketServer first, so
    // its close() still sees a live RemoteServer and executor). Held through a
    // shared_ptr the destroying thread owns, which is what keeps it alive if the
    // deadline is missed: that thread is then still inside close() and must not
    // observe these objects destroyed.
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
        owned.reset();  // ~SocketServer -> close() -> join the accept thread
        destroyed->store(true);
    }};

    if (!morph::testing::waitUntil([destroyed] { return destroyed->load(); }, std::chrono::seconds{5})) {
        destroyer.detach();  // still parked in close(); the thread keeps `owned` alive on purpose
        FAIL("SocketServer destruction did not complete within 5s: the accept loop was never unblocked (morph#437)");
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

TEST_CASE("SocketServer::listen() fails closed when the WakeupPipe can't be constructed (fd exhaustion)",
          "[net][socket_server]") {
    // listen()'s very first check (`!_wakeup.valid()`) never sees `false` in
    // any other test: WakeupPipe's constructor only fails if `::pipe()`
    // fails, which needs the process to be out of file descriptors.
    // RLIMIT_NOFILE, lowered to exactly the number of fds already open (zero
    // headroom for the two new ones `pipe()` needs), forces that
    // deterministically without touching anything already open.
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
        // Constructed under the constrained limit: WakeupPipe's pipe() call
        // had no fd headroom and must have failed.
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
    // NOTE (classification correction vs. the findings doc): `_closing`
    // defaults to `true`, so this hits the *same* early-return guard already
    // exercised by every other idempotent-close test in this file
    // (wasAlreadyClosing == true, and the accept thread -- never started --
    // is not joinable). It does not reach line 110's `_acceptThread.joinable()`
    // check with a `false` result the way the findings doc's suggested
    // reproduction implied; that branch turns out to only be reachable via
    // the genuinely concurrent close() race below. Kept anyway as a basic
    // regression check: a constructed-but-never-started server must still
    // close/destruct cleanly.
    morph::exec::ThreadPoolExecutor pool{1};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);
    morph::net::SocketServer wsServer{*server, 0};
    wsServer.close();  // must not crash or hang
    wsServer.close();  // idempotent
}

TEST_CASE("SocketServer: concurrent close() calls from two threads do not hang or corrupt state",
          "[net][socket_server]") {
    // close()'s idempotency guard (`wasAlreadyClosing && !_acceptThread.joinable()`)
    // has a narrow window: if two threads call close() at genuinely the same
    // time, thread B's check can find `wasAlreadyClosing == true` (A already
    // flipped `_closing`) but `_acceptThread.joinable() == true` (A hasn't
    // finished joining it yet) -- so B falls through the early-return guard,
    // and both threads end up calling `.join()` on the very same std::thread
    // concurrently. Confirmed via a standalone repro on this platform (Apple
    // libc++/Darwin): the loser's `.join()` throws `std::system_error`
    // ("Invalid argument") rather than hanging or corrupting memory, and the
    // winner's successful join still leaves the thread object correctly
    // non-joinable afterward (2000/2000 repro iterations, no crash). Each
    // racer below therefore wraps its close() call in try/catch: letting a
    // std::system_error escape a std::thread's initial function would call
    // std::terminate(), which is exactly the kind of "test that can crash the
    // whole binary" this finding warns about if not handled carefully.
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

    if (!morph::testing::waitUntil([done] { return done->load(); }, std::chrono::seconds{15})) {
        worker.detach();
        FAIL("Concurrent close() race did not complete within 15s -- possible hang from a double join");
    }
    worker.join();
    INFO("Observed " << exceptionCount->load() << " std::system_error(s) escaping a racing close() call across "
                     << kIterations << " iterations (the narrow join() race this test targets)");
}

TEST_CASE("SocketServer: acceptLoop's _closing checks observe a concurrent close() racing a pending connection",
          "[net][socket_server]") {
    // acceptLoop() checks `_closing.load()` twice per iteration (once right
    // after waking from poll(), once right after tryAccept()); both checks
    // are never `true` in any other test in this file, because close()'s
    // `_wakeup.signal()` is what normally wakes the loop. If a real
    // connection is already pending on the listener when close() races in,
    // poll() can report listener-readiness instead of (or as well as) the
    // wake fd, letting the loop reach either checkpoint with `_closing`
    // already flipped.
    //
    // A single connect() racing a single close() essentially never lands
    // this: close()'s wakeup write is a handful of instructions, almost
    // always faster than a fresh connect() reaching the backlog, so
    // acceptLoop()'s poll() is woken by the wake fd (not the listener) on
    // nearly every iteration -- confirmed empirically (0/many across earlier
    // revisions of this test using one real OS thread per connect() attempt).
    // fireNonBlockingConnect() below skips both the thread-per-attempt
    // overhead and TcpSocket::connect()'s getaddrinfo()/poll() machinery, so a
    // burst of many can be issued, back-to-back, on this same thread, right
    // before calling close() -- queuing several connections into the backlog
    // gives the accept loop several iterations' worth of draining to do,
    // widening the window during which a concurrent close() can land
    // mid-drain. Repeated, statistical -- matching this file's existing
    // "destruction completes promptly" precedent for #437-class races.
    //
    // A hang is the loop *stopping*, not the loop being slow -- and only the
    // first of those is a bug in SocketServer. What one iteration costs is set
    // by how many of its 16 connects acceptLoop() manages to take before
    // close() lands: each one it takes spawns a client thread that has to be
    // shut down and joined. Under Valgrind, where every one of those thread
    // switches goes through Valgrind's own serialising scheduler, the whole
    // 200-iteration loop measured 0.35 s to 1.76 s on a developer box and
    // ~44 s for the structurally identical burst test just below on a
    // GitHub-hosted runner. A wall-clock budget on the *total* therefore
    // measures the runner, not liveness, which is what made a 30 s one fire on
    // a run where nothing was stuck (morph#476).
    //
    // So: watch progress instead of the total. `progress` ticks once per
    // completed iteration; the loop is only declared hung when it stops
    // ticking, which no amount of slowness can imitate. A runner too slow to
    // finish all 200 just finishes fewer -- the race this test samples is
    // sampled on every iteration, so a short run is a weaker sample, not a
    // failure. `stop` retires the worker at the overall budget so it is always
    // joinable: detaching it left a thread constructing `RemoteServer`s after
    // `exit()` had already destroyed `allowAllAuthorizer()`'s function-local
    // static, which memcheck reports as a use-after-free -- a real error
    // manufactured by the timeout path itself.
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
        FAIL("acceptLoop/close() race stopped making progress for " << kStallBudget.count() << "s after " << lastSeen
                                                                    << " iterations -- hang");
    }
    REQUIRE(progress->load() > 0);
}

TEST_CASE("SocketServer: acceptLoop retries when a pending connection is aborted before accept() runs",
          "[net][socket_server]") {
    // tryAccept() returning nullopt (EAGAIN/EWOULDBLOCK/ECONNABORTED) means
    // "the pending connection went away before we took it" -- never exercised
    // elsewhere. A single sequential connect-then-abort essentially always
    // loses this race on this machine: the listener's own accept() is simpler
    // and faster than our own connect()+setsockopt()+close() round trip
    // (confirmed empirically -- 0/150 with one real, fully-established
    // TcpSocket::connect() per attempt, sequential or threaded). Firing many
    // *non-blocking* connects back-to-back on one thread (skipping both the
    // thread-per-attempt overhead and TcpSocket::connect()'s
    // getaddrinfo()/poll() machinery -- see fireNonBlockingConnectAndArmAbort())
    // queues a deep backlog fast enough that some entries' RST plausibly lands
    // before the single-threaded accept loop's sequential, one-at-a-time
    // accept()+thread-spawn+handshake-attempt cycle reaches them.
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

// ── Undocumented extra beyond the findings doc's 15 numbered gaps ──────────
// acceptLoop()'s `catch` around `tryAccept()` itself (lines 182-186, "listener
// is unusable (server closing)") is a *different* branch from finding #10's
// nullopt case just above: tryAccept() only returns nullopt for
// EAGAIN/EWOULDBLOCK/ECONNABORTED, but *throws* for any other accept(2)
// failure. None of the findings doc's 15 write-ups mention this catch. Unlike
// the OS-scheduling races above, EMFILE is deterministic: accept(2) checks
// the process's fd-table limit inside the kernel before returning a new fd,
// independent of any TCP-level timing -- exactly finding #1's fault-injection
// technique (RLIMIT_NOFILE), reused here against accept() instead of pipe().
TEST_CASE("SocketServer: acceptLoop's tryAccept() exception path is caught when accept() runs out of fds",
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

    // Let the accept loop's *own* poll()/tryAccept() cycle discover and fail
    // on the pending connections on its own -- deliberately not calling
    // close() yet. close()'s _wakeup.signal() would otherwise race the
    // still-pending connections' own readiness for which one poll() reports
    // first (acceptLoop() checks the wake fd's revents before the
    // listener's, so it wins whenever both are ready), and calling close() immediately
    // after firing the connects lets it win essentially every time, returning
    // via the ordinary "close() signalled" path before tryAccept() is ever
    // attempted. There is no public signal to poll for "the accept thread
    // gave up" instead, so this waits a fixed, generous duration -- EMFILE is
    // a persistent *state* here (the limit stays constrained the whole time),
    // not a narrow instant, so any reasonable wait reaches it.
    std::this_thread::sleep_for(std::chrono::milliseconds{300});

    // acceptLoop() should have observed EMFILE and returned on its own by
    // now (see the comment in socket_server.hpp: "listener is unusable").
    // Confirm the accept thread actually terminated, rather than assuming it:
    // close() should complete promptly since nothing is left parked in
    // poll().
    auto closed = std::make_shared<std::atomic<bool>>(false);
    std::thread closer([&wsServer, closed] {
        wsServer.close();
        closed->store(true);
    });
    bool const finishedPromptly =
        morph::testing::waitUntil([closed] { return closed->load(); }, std::chrono::seconds{5});

    ::setrlimit(RLIMIT_NOFILE, &original);  // restore before any further fd use, including cleanup below
    for (int const fd : clientFds) {
        ::close(fd);
    }

    if (!finishedPromptly) {
        closer.detach();
        FAIL("close() did not complete within 5s after tryAccept() should have hit EMFILE and exited the loop");
    }
    closer.join();
}

TEST_CASE("SocketServer: sendText() catches a send failure when the peer resets mid-reply", "[net][socket_server]") {
    // The classic "peer reset the connection right as the server tries to
    // reply" scenario: ClientConnection::sendText()'s try/catch around
    // sendAll(). RemoteServer::handle() dispatches the actual reply
    // asynchronously through the given IExecutor (both the top-level dispatch
    // and, for "execute", a further per-model strand post -- see
    // morph::testing::StepExecutor's own doc comment), which normally races
    // against this same connection's read side noticing a reset via
    // recvSome() (the already-tested clean-drop path). On this machine that
    // race is essentially always won by the read side when the reply runs on
    // its own thread pool (confirmed: 300/300 landed there in an earlier
    // revision of this test using ThreadPoolExecutor -- see git history),
    // because it is already blocked in recvSome() and wakes directly off the
    // very same RST that would fail the send, whereas the reply is
    // independently scheduled work with no relationship to when the reset
    // happens.
    //
    // StepExecutor removes the *scheduling* half of that race: nothing runs
    // until this test thread calls runAll(), so the reset is always applied
    // before any reply-producing work is even attempted. What remains is only
    // the much narrower race between this thread's own (synchronous, no
    // scheduling delay) dispatch-and-reply call chain and the connection's
    // real OS read-thread noticing the same RST -- heavily biased toward this
    // thread winning, but not provably 100% (confirmed empirically: usually
    // wins on the first attempt, occasionally needs a retry). A handful of
    // repeats over fresh connections closes the rest of that gap.
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
        // Wait for the connection's own clientLoop thread to have read the
        // request and posted its dispatch -- but nothing has run yet.
        REQUIRE(morph::testing::waitUntil([&] { return pool.pending() > 0; }));

        client->armAbortiveClose();
        client.reset();  // destructor -> abortive close -> RST, *before* any reply work runs

        // Runs the dispatch (and any nested strand-posted reply task) with the
        // connection already broken: sendText()'s sendAll() should fail here.
        pool.runAll();
    }

    // The server must still be usable afterward: no RST'd peer may wedge the
    // accept loop or any other connection. `pool` is a StepExecutor, so this
    // reply needs pumping too, exactly like the registers above.
    RawWsClient probe{wsServer.port()};
    probe.send(morph::wire::makeRegister("NetEchoModel"));
    REQUIRE(morph::testing::waitUntil([&] { return pool.pending() > 0; }));
    pool.runAll();
    auto probeReg = probe.receive();
    REQUIRE(probeReg.kind == "ok");
}

TEST_CASE("SocketServer: a malformed handshake is caught, the accept loop keeps serving other clients",
          "[net][socket_server]") {
    // Exactly the "malformed handshake" scenario the task brief calls out:
    // performServerHandshake() throws, clientLoop() catches it and returns
    // without crashing the accept loop or wedging other connections.
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

TEST_CASE("SocketServer: a malformed WebSocket frame is caught, the accept loop keeps serving other clients",
          "[net][socket_server]") {
    // The other headline "error path nobody bothers testing": a continuation
    // frame with no message in progress (same violation as test_ws_frame.cpp's
    // "rejects a continuation with no message in progress"), sent over a real
    // socket instead of fed to the reader directly. tryExtractFrame() throws;
    // drainFrames() catches it, marks the connection closed, and returns
    // false -- without taking down the accept loop.
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

TEST_CASE("SocketServer: sendControlFrame() swallows a send failure when the peer resets before the Pong reply",
          "[net][socket_server]") {
    // sendControlFrame()'s own try/catch (lines 263-267) is a *different*
    // catch block from sendText()'s (it never sets `closed` -- see the doc
    // comment above sendControlFrame() -- because the next read will notice
    // the same thing). Unlike sendText()'s reply, which is dispatched onto a
    // thread pool (see the test above), a control-frame reply runs
    // synchronously on the *same* clientLoop thread that just read the Ping:
    // there is no read-side thread racing it, so sending the abortive close
    // immediately after the Ping bytes reliably lands the RST before this
    // thread gets back around to attempting the Pong reply.
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

// SPDX-License-Identifier: Apache-2.0

#include <fcntl.h>
#include <poll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstring>
#include <morph/net/detail/tcp_socket.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using morph::net::detail::TcpSocket;

namespace {

/// Reads a descriptor's `O_NONBLOCK` bit. `::fcntl` is variadic by POSIX's
/// design, as it is at every call site in `tcp_socket.hpp` itself.
// NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
bool isNonBlocking(int rawFd) { return (::fcntl(rawFd, F_GETFL, 0) & O_NONBLOCK) != 0; }

/// Sets `O_NONBLOCK` on a descriptor, reporting whether it took.
bool makeNonBlocking(int rawFd) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    int const flags = ::fcntl(rawFd, F_GETFL, 0);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    return flags >= 0 && ::fcntl(rawFd, F_SETFL, flags | O_NONBLOCK) == 0;
}

}  // namespace

namespace {

// Highest fd currently open in this process, scanned up to `limit`. Same
// technique as tests/net/test_socket_server.cpp's own `highestOpenFd()`.
int highestOpenFd(int limit) {
    int highest = -1;
    for (int fd = 0; fd < limit; ++fd) {
        if (::fcntl(fd, F_GETFD) != -1) {
            highest = fd;
        }
    }
    return highest;
}

// RAII guard: forces the fd table to genuinely zero headroom and restores the
// original state on destruction.
//
// Lowering RLIMIT_NOFILE to `highestOpenFd()+1` (as
// tests/net/test_socket_server.cpp's own EMFILE tests do) assumes fd
// allocation so far has been gap-free -- true there, but not safe to assume
// in this file: an earlier TEST_CASE here can open and close a *lower*-
// numbered fd (e.g. a transient connect() attempt) while a *higher*-numbered
// one stays permanently open (this process's getaddrinfo() call opens a
// long-lived resolver connection on first use, on this platform), leaving a
// gap below the computed "highest". The very next fd-allocating syscall then
// silently reuses that gap -- allowed by RLIMIT_NOFILE, since the gap's fd
// number is still under the limit -- and the intended EMFILE never fires.
// Confirmed empirically: the naive `highest+1` version of this guard passed
// this file's [tcp] tag roughly half the time and failed the other half.
//
// This version is self-verifying instead of computed: it lowers the limit,
// then actually opens `/dev/null` repeatedly until `open()` itself fails,
// filling any such gap for real rather than assuming there isn't one. Only
// once real exhaustion has been *observed* does the syscall under test run.
class FdLimitClamp {
public:
    FdLimitClamp() {
        REQUIRE(::getrlimit(RLIMIT_NOFILE, &_original) == 0);
        int const highest = highestOpenFd(
            _original.rlim_cur < static_cast<rlim_t>(65536) ? static_cast<int>(_original.rlim_cur) : 65536);
        REQUIRE(highest >= 0);
        // A little headroom above `highest` so the fill loop below has a
        // small, bounded number of fds to open rather than racing to a huge
        // platform-default ceiling.
        rlimit constrained = _original;
        constrained.rlim_cur = static_cast<rlim_t>(highest + 17);
        REQUIRE(::setrlimit(RLIMIT_NOFILE, &constrained) == 0);

        for (;;) {
            int const fd = ::open("/dev/null", O_RDONLY);
            if (fd < 0) {
                break;
            }
            _dummyFds.push_back(fd);
        }
        // Genuinely exhausted now (confirmed by the loop above observing
        // open() itself fail), regardless of any gap in what was already open.
    }
    FdLimitClamp(const FdLimitClamp&) = delete;
    FdLimitClamp& operator=(const FdLimitClamp&) = delete;
    ~FdLimitClamp() {
        for (int const fd : _dummyFds) {
            ::close(fd);
        }
        ::setrlimit(RLIMIT_NOFILE, &_original);
    }

private:
    rlimit _original{};
    std::vector<int> _dummyFds;
};

}  // namespace

TEST_CASE("TcpSocket: listen on port 0 gets an OS-assigned port", "[net][tcp]") {
    auto listener = TcpSocket::listen(0);
    REQUIRE(listener.boundPort() != 0U);
}

TEST_CASE("TcpSocket: connect/accept/send/recv round-trip", "[net][tcp]") {
    auto listener = TcpSocket::listen(0);
    std::uint16_t const port = listener.boundPort();

    TcpSocket serverSide;
    std::thread acceptThread{[&] { serverSide = listener.accept(); }};

    auto clientSide = TcpSocket::connect("127.0.0.1", port, std::chrono::milliseconds{2000});
    acceptThread.join();
    REQUIRE(serverSide.valid());
    REQUIRE(clientSide.valid());

    std::string const message = "hello over raw tcp";
    clientSide.sendAll(message.data(), message.size());

    char buf[128];
    std::size_t total = 0;
    while (total < message.size()) {
        std::size_t got = serverSide.recvSome(buf + total, sizeof(buf) - total);
        REQUIRE(got != 0U);
        total += got;
    }
    REQUIRE(std::string(buf, total) == message);
}

TEST_CASE("TcpSocket: connect to a non-listening port fails within the timeout", "[net][tcp]") {
    // Port 1 is reserved (root-only) on Linux/macOS and never listening.
    REQUIRE_THROWS_AS(TcpSocket::connect("127.0.0.1", 1, std::chrono::milliseconds{500}), std::runtime_error);
}

TEST_CASE("TcpSocket: shutdownBoth unblocks a concurrent recvSome", "[net][tcp]") {
    auto listener = TcpSocket::listen(0);
    std::uint16_t const port = listener.boundPort();

    TcpSocket serverSide;
    std::thread acceptThread{[&] { serverSide = listener.accept(); }};
    auto clientSide = TcpSocket::connect("127.0.0.1", port, std::chrono::milliseconds{2000});
    acceptThread.join();

    std::atomic<bool> recvReturned{false};
    std::thread recvThread{[&] {
        char buf[16];
        (void)serverSide.recvSome(buf, sizeof(buf));  // blocks until shutdown
        recvReturned.store(true);
    }};
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    REQUIRE_FALSE(recvReturned.load());

    serverSide.shutdownBoth();
    recvThread.join();
    REQUIRE(recvReturned.load());
}

TEST_CASE("TcpSocket: recvSome returns 0 when the peer closes cleanly", "[net][tcp]") {
    auto listener = TcpSocket::listen(0);
    std::uint16_t const port = listener.boundPort();

    TcpSocket serverSide;
    std::thread acceptThread{[&] { serverSide = listener.accept(); }};
    {
        auto clientSide = TcpSocket::connect("127.0.0.1", port, std::chrono::milliseconds{2000});
        acceptThread.join();
        // clientSide destructs here, closing its end.
    }
    char buf[16];
    std::size_t got = serverSide.recvSome(buf, sizeof(buf));
    REQUIRE(got == 0U);
}

// ── Non-blocking listener (morph#437) ───────────────────────────────────────

TEST_CASE("TcpSocket: setNonBlocking makes an idle listener answer tryAccept with nullopt", "[net][tcp]") {
    // The property SocketServer's accept loop depends on: once poll() has
    // reported the listener readable, taking the connection must never park the
    // thread, because a peer that reset in between would leave the loop blocked
    // in a call the wakeup pipe cannot reach.
    auto listener = TcpSocket::listen(0);
    REQUIRE(listener.setNonBlocking());

    auto const started = std::chrono::steady_clock::now();
    auto accepted = listener.tryAccept();
    auto const elapsed = std::chrono::steady_clock::now() - started;

    REQUIRE_FALSE(accepted.has_value());
    // A blocking listener would still be inside ::accept here, with no client
    // in sight and nothing scheduled to produce one.
    REQUIRE(elapsed < std::chrono::seconds{1});
}

TEST_CASE("TcpSocket: tryAccept takes a pending connection on a non-blocking listener", "[net][tcp]") {
    auto listener = TcpSocket::listen(0);
    std::uint16_t const port = listener.boundPort();
    REQUIRE(listener.setNonBlocking());

    auto clientSide = TcpSocket::connect("127.0.0.1", port, std::chrono::milliseconds{2000});
    REQUIRE(clientSide.valid());

    // The connect above has completed on loopback, but the pending connection
    // reaching the accept queue is still a scheduling event; retry rather than
    // assume the first attempt wins.
    std::optional<TcpSocket> serverSide;
    for (int i = 0; i < 200 && !serverSide; ++i) {
        serverSide = listener.tryAccept();
        if (!serverSide) {
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
    }
    REQUIRE(serverSide.has_value());
    // NOLINTBEGIN(bugprone-unchecked-optional-access) — the REQUIRE above is the check
    REQUIRE(serverSide.value().valid());

    std::string const ping = "ping";
    clientSide.sendAll(ping.data(), ping.size());
    std::array<char, 16> buf{};
    std::size_t const got = serverSide.value().recvSome(buf.data(), buf.size());
    // NOLINTEND(bugprone-unchecked-optional-access)
    REQUIRE(std::string(buf.data(), got) == ping);
}

TEST_CASE("TcpSocket: setNonBlocking reports failure on an empty socket", "[net][tcp]") {
    TcpSocket empty;
    REQUIRE_FALSE(empty.setNonBlocking());
}

// ── Adopted sockets are blocking (morph#478) ────────────────────────────────

TEST_CASE("TcpSocket: adopting a non-blocking descriptor clears O_NONBLOCK", "[net][tcp]") {
    // The regression control for morph#478, and the only test in this file that
    // can fail on Linux because of it.
    //
    // macOS/BSD propagate a listener's O_NONBLOCK onto the sockets accept(2)
    // returns; Linux does not (measured with a standalone accept() probe here:
    // "accepted fd O_NONBLOCK inherited from listener: NO"). So on this
    // platform accept() never yields a non-blocking socket, and a test that
    // merely accepts a connection and inspects the result would pass with the
    // fix reverted -- a control measuring nothing, which
    // docs/spec/testing_charter.md exists to keep out of this tree.
    //
    // What is platform-independent is the rule the fix installs: the
    // fd-adopting constructor is the single place blocking mode is decided, and
    // every accepted socket reaches it. Handing that constructor a descriptor
    // that really is non-blocking is how to observe the decision on a platform
    // whose accept() cannot produce one. Remove the clearNonBlocking() call and
    // this fails here, today.
    int const rawFd = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(rawFd >= 0);
    REQUIRE(makeNonBlocking(rawFd));
    REQUIRE(isNonBlocking(rawFd));

    TcpSocket const adopted{rawFd};  // takes ownership; closes it at scope exit

    REQUIRE(adopted.nativeHandle() == rawFd);
    REQUIRE_FALSE(isNonBlocking(rawFd));
}

TEST_CASE("TcpSocket: tryAccept hands back a blocking connection", "[net][tcp]") {
    // The contract SocketServer::clientLoop() depends on: recvSome() treats
    // EAGAIN as fatal, and the first thing clientLoop() does with an accepted
    // socket is performServerHandshake(), which reads before the client's
    // Upgrade bytes have necessarily arrived. A non-blocking accepted socket
    // therefore fails every connection (morph#478).
    //
    // Stated limit: on Linux this assertion also holds with the fix reverted,
    // because accept() here never produces a non-blocking socket to begin with.
    // It records the end-to-end property and fails on a platform that does
    // propagate; the constructor test above is the control that fails here.
    auto listener = TcpSocket::listen(0);
    std::uint16_t const port = listener.boundPort();
    REQUIRE(listener.setNonBlocking());

    auto clientSide = TcpSocket::connect("127.0.0.1", port, std::chrono::milliseconds{2000});
    REQUIRE(clientSide.valid());

    std::optional<TcpSocket> serverSide;
    for (int i = 0; i < 200 && !serverSide; ++i) {
        serverSide = listener.tryAccept();
        if (!serverSide) {
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
    }
    REQUIRE(serverSide.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) — the REQUIRE above is the check
    REQUIRE_FALSE(isNonBlocking(serverSide.value().nativeHandle()));
}

TEST_CASE("TcpSocket: accept hands back a blocking connection", "[net][tcp]") {
    // accept() has the same body shape as tryAccept() -- `return
    // TcpSocket{clientFd}` with nothing done to the fd -- so it carried the
    // same hole, and the constructor closes both. It is reachable on a
    // non-blocking listener whenever a connection is already queued, which is
    // what this sets up.
    auto listener = TcpSocket::listen(0);
    std::uint16_t const port = listener.boundPort();
    REQUIRE(listener.setNonBlocking());

    auto clientSide = TcpSocket::connect("127.0.0.1", port, std::chrono::milliseconds{2000});
    REQUIRE(clientSide.valid());

    // Wait for the connection to reach the accept queue: on a non-blocking
    // listener accept() would throw EAGAIN rather than park.
    pollfd pfd{};
    pfd.fd = listener.nativeHandle();
    pfd.events = POLLIN;
    REQUIRE(::poll(&pfd, 1, 2000) == 1);

    auto const serverSide = listener.accept();
    REQUIRE(serverSide.valid());
    REQUIRE_FALSE(isNonBlocking(serverSide.nativeHandle()));
}

// ── Task 6c: closing tcp_socket.hpp's remaining coverage gaps ──────────────
// See .superpowers/sdd/2026-09-03-framework-coverage-and-mutation/
// task-6-tcp_socket-findings.md for the audit this section closes.

TEST_CASE("TcpSocket: self-move-assignment leaves the socket valid", "[net][tcp]") {
    auto sock = TcpSocket::listen(0);
    std::uint16_t const port = sock.boundPort();
    // Routed through a pointer so this isn't the textually-identical
    // `sock = std::move(sock)` that -Weverything's -Wself-move would flag.
    TcpSocket* self = &sock;
    sock = std::move(*self);
    REQUIRE(sock.valid());
    REQUIRE(sock.boundPort() == port);
}

TEST_CASE("TcpSocket::connect: rejects a malformed host without a DNS round-trip", "[net][tcp]") {
    // A hostname past RFC 1035's length limit fails without a DNS round-trip
    // (EAI_NONAME, ~3ms on this machine) -- deterministic and portable,
    // unlike a name that merely looks made up (still routed through a real,
    // possibly slow, DNS lookup). It does still touch the local resolver on
    // this platform, which is exactly where this file's permanent resolver
    // fd comes from -- see FdLimitClamp's comment above.
    std::string const tooLong(300, 'x');
    REQUIRE_THROWS_AS(TcpSocket::connect(tooLong, 80, std::chrono::milliseconds{500}), std::runtime_error);
}

TEST_CASE("TcpSocket::connect: fails cleanly when a black-holed peer never completes the handshake", "[net][tcp]") {
    // 10.255.255.1 is a conventional "nobody's there, nothing refuses"
    // address on a private network: connect() goes EINPROGRESS, and poll()
    // genuinely times out rather than ever observing SO_ERROR set (the
    // "connect to a non-listening port" test above gets an immediate
    // ECONNREFUSED instead, which is a different branch). This is
    // environment-dependent (a sandbox that instantly rejects all outbound
    // traffic would take a different, already-covered path instead) --
    // kept permissive (just REQUIRE_THROWS_AS, no timing assertion) so it
    // cannot fail outright even where that assumption does not hold.
    REQUIRE_THROWS_AS(TcpSocket::connect("10.255.255.1", 1, std::chrono::milliseconds{300}), std::runtime_error);
}

TEST_CASE("TcpSocket::connect: fails cleanly when socket() runs out of file descriptors", "[net][tcp]") {
    {
        FdLimitClamp const clamp;
        REQUIRE_THROWS_AS(TcpSocket::connect("127.0.0.1", 54321, std::chrono::milliseconds{200}), std::runtime_error);
    }
    // The constraint was scoped to just that one call: a normal connect()
    // still works once the limit is lifted.
    auto listener = TcpSocket::listen(0);
    auto sanity = TcpSocket::connect("127.0.0.1", listener.boundPort(), std::chrono::milliseconds{2000});
    REQUIRE(sanity.valid());
}

TEST_CASE("TcpSocket::listen: fails cleanly when socket() runs out of file descriptors", "[net][tcp]") {
    {
        FdLimitClamp const clamp;
        REQUIRE_THROWS_AS(TcpSocket::listen(0), std::runtime_error);
    }
    // The constraint was scoped to just that one call: a normal listen()
    // still works once the limit is lifted.
    auto sanity = TcpSocket::listen(0);
    REQUIRE(sanity.boundPort() != 0U);
}

TEST_CASE("TcpSocket::listen: fails with EADDRINUSE when the port is already bound", "[net][tcp]") {
    auto first = TcpSocket::listen(0);
    std::uint16_t const port = first.boundPort();
    REQUIRE_THROWS_AS(TcpSocket::listen(port), std::runtime_error);
}

TEST_CASE("TcpSocket::boundPort: returns 0 on an empty socket", "[net][tcp]") {
    TcpSocket const empty;
    REQUIRE(empty.boundPort() == 0U);
}

TEST_CASE("TcpSocket::accept: throws when accept() itself runs out of file descriptors", "[net][tcp]") {
    auto listener = TcpSocket::listen(0);
    std::uint16_t const port = listener.boundPort();
    // A fully-established connection sitting in the backlog, unaccepted:
    // accept() below has something to return without blocking, so EMFILE
    // (not a wait) is what it actually observes.
    auto clientSide = TcpSocket::connect("127.0.0.1", port, std::chrono::milliseconds{2000});

    FdLimitClamp const clamp;
    REQUIRE_THROWS_AS(listener.accept(), std::runtime_error);
}

TEST_CASE("TcpSocket::recvSome: throws for a real socket error distinct from ECONNRESET", "[net][tcp]") {
    auto listener = TcpSocket::listen(0);
    std::uint16_t const port = listener.boundPort();
    TcpSocket serverSide;
    std::thread acceptThread{[&] { serverSide = listener.accept(); }};
    auto clientSide = TcpSocket::connect("127.0.0.1", port, std::chrono::milliseconds{2000});
    acceptThread.join();

    // SO_RCVTIMEO makes a blocking recv() return -1/EAGAIN once the timeout
    // elapses with nothing to read -- a real recv(2) failure recvSome() does
    // not special-case (only EINTR and ECONNRESET are), so it must reach the
    // generic throw. Deterministic: no fault injection, no OS-timing race.
    timeval tv{};
    tv.tv_sec = 0;
    tv.tv_usec = 100'000;  // 100ms
    REQUIRE(::setsockopt(serverSide.nativeHandle(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0);

    char buf[16];
    REQUIRE_THROWS_AS(serverSide.recvSome(buf, sizeof(buf)), std::runtime_error);
}

TEST_CASE("TcpSocket::sendAll: throws when the peer resets the connection", "[net][tcp]") {
    // SO_LINGER{on, 0} arms an abortive close: destroying clientSide sends a
    // real RST instead of a normal FIN. sendAll() does not special-case
    // ECONNRESET/EPIPE the way recvSome() does, so the very next send hits
    // the generic throw. Confirmed reliable (30/30) in isolation on this
    // machine; a handful of retries closes the residual scheduling gap
    // between the RST arriving and this thread's own send(2) call, matching
    // this file's other OS-timing-dependent tests.
    auto listener = TcpSocket::listen(0);
    std::uint16_t const port = listener.boundPort();

    TcpSocket serverSide;
    std::thread acceptThread{[&] { serverSide = listener.accept(); }};
    {
        auto clientSide = TcpSocket::connect("127.0.0.1", port, std::chrono::milliseconds{2000});
        acceptThread.join();
        struct linger l{};
        l.l_onoff = 1;
        l.l_linger = 0;
        ::setsockopt(clientSide.nativeHandle(), SOL_SOCKET, SO_LINGER, &l, sizeof(l));
        // clientSide destructs here -> abortive close -> RST sent to serverSide.
    }

    bool threw = false;
    char const data[] = "x";
    for (int attempt = 0; attempt < 20 && !threw; ++attempt) {
        try {
            serverSide.sendAll(data, sizeof(data));
        } catch (const std::runtime_error&) {
            threw = true;
        }
        if (!threw) {
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
    }
    REQUIRE(threw);
}

TEST_CASE("TcpSocket::shutdownBoth: a safe no-op on an empty socket", "[net][tcp]") {
    TcpSocket empty;
    empty.shutdownBoth();  // must not crash
    REQUIRE_FALSE(empty.valid());
}

TEST_CASE("WakeupPipe: construction fails cleanly when pipe() runs out of file descriptors", "[net][tcp]") {
    {
        FdLimitClamp const clamp;
        WakeupPipe const wake;
        // pipe() itself needs two fresh fds, unlike fcntl()'s F_GETFL/F_SETFL
        // (see the "left open" notes in the report for findings #10/#16/#21):
        // this is the one WakeupPipe/TcpSocket fd-allocating call this
        // technique actually reaches.
        REQUIRE_FALSE(wake.valid());

        // Both guarded no-ops on an invalid pipe -- never exercised by any
        // other test, since every other WakeupPipe in this file constructs
        // successfully.
        wake.signal();
        wake.drain();
    }
    // The constraint was scoped to just that one construction: a normal
    // WakeupPipe still works once the limit is lifted.
    WakeupPipe const sanity;
    REQUIRE(sanity.valid());
}

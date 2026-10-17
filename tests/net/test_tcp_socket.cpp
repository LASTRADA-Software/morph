// SPDX-License-Identifier: Apache-2.0

#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <exception>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <morph/net/detail/tcp_socket.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
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

// What a scan of this process's fd table found, up to some ceiling: the
// highest fd open, and how many fds in that range are open at all. Same
// technique as tests/net/test_socket_server.cpp's own `highestOpenFd()`,
// with the count added -- `highest + 1 - open` is the size of the gap that
// `FdLimitClamp` exists to fill, and it is *reported* rather than assumed
// away.
struct FdScan {
    int highest = -1;
    int open = 0;
};

FdScan scanOpenFds(int limit) {
    FdScan scan;
    for (int fd = 0; fd < limit; ++fd) {
        if (::fcntl(fd, F_GETFD) != -1) {
            scan.highest = fd;
            ++scan.open;
        }
    }
    return scan;
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
//
// This test fails under concurrent machine load if the clamp does not hold, so
// the clamp *says* what it measured rather than leaving the next reader
// guessing. `exhausted()` and `summary()` are that: every call site asserts
// `exhausted()` before the syscall under test -- so a clamp that did not bite
// fails on its own terms instead of being mistaken for a bug in `accept()` --
// and `INFO(summary())` puts the whole measurement (ambient fd table, limit
// applied, fds it took to fill, and the errno the fill loop stopped on) into
// the failure output of whichever assertion follows.
//
// The `exhausted()` check is deliberately *not* a REQUIRE inside this
// constructor: a Catch2 assertion failure there throws out of a half-built
// object, whose destructor never runs, leaving the process clamped and the
// filler fds leaked for every test after it.
class FdLimitClamp {
public:
    FdLimitClamp() {
        REQUIRE(::getrlimit(RLIMIT_NOFILE, &_original) == 0);
        _ambient = scanOpenFds(_original.rlim_cur < static_cast<rlim_t>(65536) ? static_cast<int>(_original.rlim_cur)
                                                                               : 65536);
        REQUIRE(_ambient.highest >= 0);
        // A little headroom above `highest` so the fill loop below has a
        // small, bounded number of fds to open rather than racing to a huge
        // platform-default ceiling.
        rlimit constrained = _original;
        constrained.rlim_cur = static_cast<rlim_t>(_ambient.highest + 17);
        _clampedTo = constrained.rlim_cur;
        REQUIRE(::setrlimit(RLIMIT_NOFILE, &constrained) == 0);

        for (;;) {
            int const fd = ::open("/dev/null", O_RDONLY);
            if (fd < 0) {
                _fillErrno = errno;
                break;
            }
            _dummyFds.push_back(fd);
        }
        // Genuinely exhausted now (confirmed by the loop above observing
        // open() itself fail), regardless of any gap in what was already open.
    }
    FdLimitClamp(const FdLimitClamp&) = delete;
    FdLimitClamp& operator=(const FdLimitClamp&) = delete;
    FdLimitClamp(FdLimitClamp&&) = delete;
    FdLimitClamp& operator=(FdLimitClamp&&) = delete;
    ~FdLimitClamp() {
        for (int const fd : _dummyFds) {
            ::close(fd);
        }
        ::setrlimit(RLIMIT_NOFILE, &_original);
    }

    /// `true` only if the fill loop stopped because the fd table was full.
    /// Any other stopping errno means the syscall under test is about to run
    /// against an fd table that still has headroom, so whatever it does next
    /// measures nothing.
    [[nodiscard]] bool exhausted() const { return _fillErrno == EMFILE; }

    /// Everything the clamp measured, on one line, for `INFO()` at a call
    /// site: the ambient fd table it found, the limit it applied, how many
    /// fds it had to open to fill the table, and why the fill loop stopped.
    [[nodiscard]] std::string summary() const {
        return "FdLimitClamp: ambient highest fd=" + std::to_string(_ambient.highest) +
               " open fds=" + std::to_string(_ambient.open) +
               " gap=" + std::to_string(_ambient.highest + 1 - _ambient.open) + "; RLIMIT_NOFILE soft " +
               std::to_string(_original.rlim_cur) + " -> " + std::to_string(_clampedTo) +
               "; filler fds opened=" + std::to_string(_dummyFds.size()) +
               "; fill loop stopped on errno=" + std::to_string(_fillErrno) + " (" +
               std::system_category().message(_fillErrno) + ")";
    }

private:
    rlimit _original{};
    FdScan _ambient{};
    rlim_t _clampedTo = 0;
    int _fillErrno = 0;
    std::vector<int> _dummyFds;
};

}  // namespace

TEST_CASE("TcpSocket: listen on port 0 gets an OS-assigned port", "[net][tcp]") {
    auto listener = TcpSocket::listen(0);
    REQUIRE(listener.boundPort() != 0U);
}

// ── Why the blocking accept()s below carry no deadline of their own ─────────
//
// A run can park indefinitely in `__accept` with nothing in
// the process able to satisfy it. The one site in these tests with
// that shape is bounded: `FakeWsServer::acceptAndHandshake()` in test_socket_backend.cpp,
// where the *main test thread* blocks in `accept()` while the only thing that
// could satisfy it is the I/O loop of the `SocketBackend` under test -- i.e.
// the very component whose failure to connect those tests exist to provoke.
//
// The six blocking `accept()`s in this file have the opposite shape, and it is
// the shape, not a timeout, that keeps them bounded:
//
//     std::thread acceptThread{[&] { serverSide = listener.accept(); }};
//     auto clientSide = TcpSocket::connect("127.0.0.1", port, 2000ms);   // (1)
//     acceptThread.join();                                               // (2)
//
// `accept()` runs on a helper thread; the connection that satisfies it is made
// by the *main* thread at (1), under `connect()`'s own deadline -- which
// throws rather than returning on expiry -- and that happens before the main
// thread waits for anything at (2). So the `join()` is only ever reached with
// a connection already established against this listener: loopback, a 64-deep
// backlog, one consumer, nothing that can take it away. A deadline here would
// be a path nothing can take, and an untakeable timeout path is worse than
// none: it reads as a hazard that was found and handled.
//
// If (1) throws instead, the helper thread stays parked and `~std::thread`
// calls `std::terminate` -- an abort in 0.09s that names the test, not a hang.
// The error it discards while doing so is not reported anywhere, which is a
// known cost of this shape.
//
// **Verification status.** The reasoning is inferred from reading plus
// `tcp_socket.hpp`'s own `accept()` contract; it is not a measurement that
// these sites cannot park. What *was* measured, on this file at 80e6ad74 (gcc
// 16.2.1 Debug, Linux), is that (1) is the load-bearing step. Redirect it to a
// second, decoy listener -- so `connect()` still succeeds and this listener's
// `accept()` is never satisfied -- and the test parks in `join()` until ctest's
// own per-test cap fires:
//
//     1/1 Test #1696: TcpSocket: connect/accept/send/recv round-trip ...***Timeout 120.08 sec
//     The following tests FAILED:
//     	1696 - TcpSocket: connect/accept/send/recv round-trip (Timeout) net
//
// That is also the backstop if this reasoning is ever wrong: `TIMEOUT 120` in
// tests/net/CMakeLists.txt turns a park into a named ctest failure. It is a
// worse name than a bounded accept gives (`accept()` timed out, versus "this
// test was slow") and six times the wall clock, which is why a site that genuinely can
// starve gets its own bound -- and why a site that cannot does not.
//
// Also bounded, and the two exceptions to the pattern above, both `accept()`ing
// on the main thread: "accept hands back a blocking connection" gates its
// `accept()` on a 2s `poll()` over a *non-blocking* listener, where `accept()`
// fails with EAGAIN rather than parking; and the EMFILE test gates its own on a
// 5s `poll()` that both establishes the precondition it used to assume and
// bounds the wait.

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
    // Bounded by shape rather than by a deadline: see the note above the round-trip test.
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
    // Bounded by shape rather than by a deadline: see the note above the round-trip test.
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

// ── Non-blocking listener ───────────────────────────────────────────────────

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

// ── Adopted sockets are blocking ────────────────────────────────────────────

TEST_CASE("TcpSocket: adopting a non-blocking descriptor clears O_NONBLOCK", "[net][tcp]") {
    // The regression control for the adopted-descriptor rule, and the only test
    // in this file that can fail on Linux because of it.
    //
    // macOS/BSD propagate a listener's O_NONBLOCK onto the sockets accept(2)
    // returns; Linux does not (measured with a standalone accept() probe here:
    // "accepted fd O_NONBLOCK inherited from listener: NO"). So on this
    // platform accept() never yields a non-blocking socket, and a test that
    // merely accepts a connection and inspects the result would pass with the
    // fix reverted -- a control measuring nothing, which
    // AGENTS.md's "Verify rather than assert" exists to keep out of this tree.
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
    // The contract a blocking reader of an accepted socket depends on:
    // recvSome() treats EAGAIN as fatal, and performServerHandshake() reads
    // before the client's Upgrade bytes have necessarily arrived. A
    // non-blocking accepted socket therefore fails every such connection.
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
        INFO(clamp.summary());
        REQUIRE(clamp.exhausted());
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
        INFO(clamp.summary());
        REQUIRE(clamp.exhausted());
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

// Pins the *shape* of a socket error message, and pins it to the category that
// renders it rather than to a literal string. Every throw site in
// `tcp_socket.hpp` formats its message on whichever thread hit the error, and
// this subsystem spawns those threads itself, so the renderer has to be one
// that two threads may call at once -- `std::error_category::message`, not
// `std::strerror`.
//
// What this case does not establish: that `std::strerror` would actually race
// here. glibc renders both spellings to the same bytes, so this assertion
// holds either way. The evidence for the choice is clang-tidy
// `concurrency-mt-unsafe`, which reports six findings in this header for the
// unsafe spelling and none for this one; this case is a standing guard on the
// message, not that measurement.
TEST_CASE("TcpSocket::listen renders a bind() failure through std::system_category", "[net][tcp]") {
    auto first = TcpSocket::listen(0);
    std::uint16_t const port = first.boundPort();
    REQUIRE_THROWS_WITH(TcpSocket::listen(port), Catch::Matchers::Equals("TcpSocket::listen: bind() failed: " +
                                                                         std::system_category().message(EADDRINUSE)));
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

    // ...but that is a *precondition*, not something `connect()` returning
    // establishes on its own. A loopback `connect()` completes when the
    // SYN-ACK arrives, which is a different instant from the one at which the
    // listener's accept queue gains the child socket (the final ACK). On a
    // machine under load those two can separate, and the blocking `accept()`
    // below would then park -- a net test parked in `accept()`
    // indefinitely takes a whole run with it. Waiting for the listener to
    // actually report readable turns that into a bounded, named failure here,
    // and leaves `accept()` with a connection genuinely queued so the EMFILE
    // it hits is the one under test.
    pollfd listenerReady{};
    listenerReady.fd = listener.nativeHandle();
    listenerReady.events = POLLIN;
    int const queued = ::poll(&listenerReady, 1, 5000);
    INFO("poll() on the listener returned " << queued << " (revents=" << listenerReady.revents << ")");
    REQUIRE(queued == 1);

    FdLimitClamp const clamp;
    INFO(clamp.summary());
    REQUIRE(clamp.exhausted());
    REQUIRE_THROWS_AS(listener.accept(), std::runtime_error);
}

TEST_CASE("TcpSocket::recvSome: throws for a real socket error distinct from ECONNRESET", "[net][tcp]") {
    auto listener = TcpSocket::listen(0);
    std::uint16_t const port = listener.boundPort();
    TcpSocket serverSide;
    // Bounded by shape rather than by a deadline: see the note above the round-trip test.
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
    // Bounded by shape rather than by a deadline: see the note above the round-trip test.
    std::thread acceptThread{[&] { serverSide = listener.accept(); }};
    {
        auto clientSide = TcpSocket::connect("127.0.0.1", port, std::chrono::milliseconds{2000});
        acceptThread.join();
        linger l{};
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

// ── A peer that stops reading must not park the sender forever ─────────────
//
// Once the kernel send buffer fills against a peer that never reads, a blocking
// `::send` never returns -- and `sendAll` loops on it. `SocketBackend::sendFrame`
// holds `_socketMtx` across that call, so `~SocketBackend` parks on the same
// lock with nothing able to release it. (Removing the lock is NOT the fix: it
// races `onDisconnected()` reassigning `_socket`, 25 ThreadSanitizer reports.)
//
// `SO_SNDTIMEO` bounds one no-progress send instead, so `sendAll` throws the way
// it already does for any other send error and the lock is released.
TEST_CASE("TcpSocket: setSendTimeout bounds a send against a peer that never reads", "[net][tcp][morph506]") {
    auto listener = TcpSocket::listen(0);
    std::uint16_t const port = listener.boundPort();

    // Accepts and then does nothing at all -- never reads a byte. Held open for
    // the duration of the test so the connection stays established.
    TcpSocket serverSide;
    // Bounded by shape rather than by a deadline: see the note above the round-trip test.
    std::thread acceptThread{[&] { serverSide = listener.accept(); }};
    auto clientSide = TcpSocket::connect("127.0.0.1", port, std::chrono::milliseconds{2000});
    acceptThread.join();
    REQUIRE(serverSide.valid());

    constexpr auto kTimeout = std::chrono::milliseconds{300};
    REQUIRE(clientSide.setSendTimeout(kTimeout));

    // Push until the buffers fill. Without the timeout this loop never returns;
    // with it, sendAll throws once a single send makes no progress.
    std::string const chunk(std::size_t{256} * 1024, 'x');
    auto const start = std::chrono::steady_clock::now();
    bool threw = false;
    for (int i = 0; i < 400 && !threw; ++i) {
        try {
            clientSide.sendAll(chunk.data(), chunk.size());
        } catch (const std::runtime_error&) {
            threw = true;
        }
    }
    auto const elapsed = std::chrono::steady_clock::now() - start;

    REQUIRE(threw);
    // The bound that matters: it gave up rather than blocking indefinitely.
    // Generous multiple of the timeout, since each successful send before the
    // buffers filled costs real time too.
    CHECK(elapsed < std::chrono::seconds{20});
}

// ── pollUntil: one budget for the whole wait ────────────────────────────────

TEST_CASE("pollUntil: a deadline already passed reports kTimedOut without waiting", "[net][tcp][poll_until]") {
    auto listener = TcpSocket::listen(0);
    auto const started = std::chrono::steady_clock::now();
    auto const result = morph::net::detail::pollUntil(listener.nativeHandle(), POLLIN, started);
    CHECK(result.outcome == morph::net::detail::PollOutcome::kTimedOut);
    CHECK(result.error == 0);
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::milliseconds{100});
}

TEST_CASE("pollUntil: an idle listener times out at the deadline", "[net][tcp][poll_until]") {
    auto listener = TcpSocket::listen(0);
    auto const started = std::chrono::steady_clock::now();
    auto const result =
        morph::net::detail::pollUntil(listener.nativeHandle(), POLLIN, started + std::chrono::milliseconds{100});
    auto const elapsed = std::chrono::steady_clock::now() - started;
    CHECK(result.outcome == morph::net::detail::PollOutcome::kTimedOut);
    CHECK(elapsed >= std::chrono::milliseconds{100});
    CHECK(elapsed < std::chrono::seconds{5});
}

TEST_CASE("pollUntil: a pending connection reports kReady", "[net][tcp][poll_until]") {
    auto listener = TcpSocket::listen(0);
    auto clientSide = TcpSocket::connect("127.0.0.1", listener.boundPort(), std::chrono::milliseconds{2000});
    REQUIRE(clientSide.valid());
    auto const result = morph::net::detail::pollUntil(listener.nativeHandle(), POLLIN,
                                                      std::chrono::steady_clock::now() + std::chrono::seconds{5});
    CHECK(result.outcome == morph::net::detail::PollOutcome::kReady);
}

namespace {
void pollUntilTestNoopHandler(int /*signal*/) {}
}  // namespace

TEST_CASE("pollUntil: signals mid-wait neither end the wait nor re-arm the full budget", "[net][tcp][poll_until]") {
    // SIGUSR1 aimed at this thread every 50 ms, with no SA_RESTART, interrupts
    // poll() with EINTR over and over for 1.5 s. The wait must still end at its
    // own 300 ms deadline: a loop that treated EINTR as failure would report
    // kFailed, and one that re-armed the full budget after each signal would
    // not finish until the signals stopped.
    struct sigaction action{};
    action.sa_handler = pollUntilTestNoopHandler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    struct sigaction previous{};
    REQUIRE(::sigaction(SIGUSR1, &action, &previous) == 0);

    auto listener = TcpSocket::listen(0);
    pthread_t const waiter = ::pthread_self();
    std::atomic<int> sent{0};
    std::thread interrupter{[&] {
        for (int i = 0; i < 30; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
            ::pthread_kill(waiter, SIGUSR1);
            sent.fetch_add(1);
        }
    }};

    auto const started = std::chrono::steady_clock::now();
    auto const result =
        morph::net::detail::pollUntil(listener.nativeHandle(), POLLIN, started + std::chrono::milliseconds{300});
    auto const elapsed = std::chrono::steady_clock::now() - started;
    int const sentDuringWait = sent.load();
    interrupter.join();
    ::sigaction(SIGUSR1, &previous, nullptr);

    REQUIRE(sentDuringWait > 0);
    CHECK(result.outcome == morph::net::detail::PollOutcome::kTimedOut);
    CHECK(elapsed >= std::chrono::milliseconds{300});
    CHECK(elapsed < std::chrono::milliseconds{1200});
}

// ── Signals delivered to a thread parked in a socket call ───────────────────
//
// Each case parks a helper thread in one blocking call, confirms from
// /proc/self/task/<tid>/syscall that the thread is inside that very syscall,
// and only then aims SIGUSR2 at it. The handler is installed without
// SA_RESTART, so the kernel ends the call with EINTR instead of restarting it:
// what the call does next is the behaviour under test. Linux-only: it reads
// the kernel's per-thread syscall report.

#ifdef __linux__
namespace {

// A signal handler can reach nothing but a global.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic<int> gSocketInterrupts{0};

void countSocketInterrupt(int /*signal*/) { gSocketInterrupts.fetch_add(1); }

/// SIGUSR2 -> countSocketInterrupt, without SA_RESTART, for the guard's lifetime.
class InterruptingSignal {
public:
    InterruptingSignal() {
        struct sigaction action{};
        action.sa_handler = countSocketInterrupt;
        sigemptyset(&action.sa_mask);
        action.sa_flags = 0;
        _installed = ::sigaction(SIGUSR2, &action, &_previous) == 0;
    }
    InterruptingSignal(const InterruptingSignal&) = delete;
    InterruptingSignal& operator=(const InterruptingSignal&) = delete;
    InterruptingSignal(InterruptingSignal&&) = delete;
    InterruptingSignal& operator=(InterruptingSignal&&) = delete;
    ~InterruptingSignal() {
        if (_installed) {
            ::sigaction(SIGUSR2, &_previous, nullptr);
        }
    }
    [[nodiscard]] bool installed() const { return _installed; }

private:
    struct sigaction _previous{};
    bool _installed = false;
};

/// The syscall number thread @p tid is blocked in, or -1 while it runs (or
/// when the kernel will not say).
long currentSyscall(pid_t tid) {
    std::ifstream file{"/proc/self/task/" + std::to_string(tid) + "/syscall"};
    std::string first;
    if (!(file >> first) || first == "running") {
        return -1;
    }
    try {
        return std::stol(first);
    } catch (const std::exception&) {
        return -1;
    }
}

/// Waits up to 10s for thread @p tid to block in one of @p syscalls.
bool waitUntilParkedIn(pid_t tid, std::initializer_list<long> syscalls) {
    for (int i = 0; i < 1000; ++i) {
        long const current = currentSyscall(tid);
        if (std::ranges::find(syscalls, current) != syscalls.end()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    return false;
}

/// Sends SIGUSR2 to @p thread and waits up to 10s for its handler to have run.
bool interrupt(std::thread& thread) {
    int const before = gSocketInterrupts.load();
    if (::pthread_kill(thread.native_handle(), SIGUSR2) != 0) {
        return false;
    }
    for (int i = 0; i < 1000 && gSocketInterrupts.load() == before; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    return gSocketInterrupts.load() != before;
}

}  // namespace

// Mutation: throw on EINTR in TcpSocket::accept instead of retrying (the parked accept reports the signal as a
// failure and the connection is never taken).
TEST_CASE("TcpSocket::accept: a signal while parked is retried, and the connection still taken", "[net][tcp][eintr]") {
    InterruptingSignal const signal;
    REQUIRE(signal.installed());
    auto listener = TcpSocket::listen(0);

    std::atomic<pid_t> tid{0};
    TcpSocket serverSide;
    std::exception_ptr failure;
    std::thread acceptThread{[&] {
        tid = ::gettid();
        try {
            serverSide = listener.accept();
        } catch (...) {
            failure = std::current_exception();
        }
    }};
    while (tid.load() == 0) {
        std::this_thread::yield();
    }
    bool const parked = waitUntilParkedIn(tid.load(), {SYS_accept, SYS_accept4});
    bool const interrupted = parked && interrupt(acceptThread);
    // Bounded by shape: this connect is what satisfies the accept.
    auto const clientSide = TcpSocket::connect("127.0.0.1", listener.boundPort(), std::chrono::milliseconds{2000});
    acceptThread.join();

    REQUIRE(parked);
    REQUIRE(interrupted);
    REQUIRE(failure == nullptr);
    CHECK(serverSide.valid());
}

// Mutation: throw on EINTR in TcpSocket::recvSome instead of retrying (the read reports the signal as a failure
// rather than returning the bytes the peer sends next).
TEST_CASE("TcpSocket::recvSome: a signal while parked is retried, and the bytes still read", "[net][tcp][eintr]") {
    InterruptingSignal const signal;
    REQUIRE(signal.installed());
    auto listener = TcpSocket::listen(0);
    TcpSocket serverSide;
    std::thread acceptThread{[&] { serverSide = listener.accept(); }};
    auto clientSide = TcpSocket::connect("127.0.0.1", listener.boundPort(), std::chrono::milliseconds{2000});
    acceptThread.join();

    std::atomic<pid_t> tid{0};
    std::string received;
    std::exception_ptr failure;
    std::thread readThread{[&] {
        tid = ::gettid();
        try {
            std::array<char, 16> buf{};
            std::size_t const got = serverSide.recvSome(buf.data(), buf.size());
            received.assign(buf.data(), got);
        } catch (...) {
            failure = std::current_exception();
        }
    }};
    while (tid.load() == 0) {
        std::this_thread::yield();
    }
    bool const parked = waitUntilParkedIn(tid.load(), {SYS_recvfrom});
    bool const interrupted = parked && interrupt(readThread);
    std::string const message = "hi";
    clientSide.sendAll(message.data(), message.size());
    readThread.join();

    REQUIRE(parked);
    REQUIRE(interrupted);
    REQUIRE(failure == nullptr);
    CHECK(received == message);
}

// Mutation: throw on EINTR in TcpSocket::sendAll instead of retrying (the write stops part-way through and the peer
// never receives the whole buffer).
TEST_CASE("TcpSocket::sendAll: signals while parked on a full buffer still deliver every byte", "[net][tcp][eintr]") {
    InterruptingSignal const signal;
    REQUIRE(signal.installed());
    auto listener = TcpSocket::listen(0);
    TcpSocket serverSide;
    std::thread acceptThread{[&] { serverSide = listener.accept(); }};
    auto clientSide = TcpSocket::connect("127.0.0.1", listener.boundPort(), std::chrono::milliseconds{2000});
    acceptThread.join();
    int const small = 4096;
    ::setsockopt(clientSide.nativeHandle(), SOL_SOCKET, SO_SNDBUF, &small, sizeof(small));
    ::setsockopt(serverSide.nativeHandle(), SOL_SOCKET, SO_RCVBUF, &small, sizeof(small));

    std::string payload(std::size_t{1024} * 1024, '\0');
    for (std::size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<char>('a' + static_cast<char>(i % 26));
    }
    std::atomic<pid_t> tid{0};
    std::exception_ptr failure;
    std::thread sendThread{[&] {
        tid = ::gettid();
        try {
            clientSide.sendAll(payload.data(), payload.size());
        } catch (...) {
            failure = std::current_exception();
        }
        // Ends the reader below at whatever was delivered, whole or not.
        clientSide.shutdownBoth();
    }};
    while (tid.load() == 0) {
        std::this_thread::yield();
    }
    // The first signal can land on a send that has already copied part of its
    // buffer, which returns that count rather than EINTR. The second lands on
    // the send that follows, which blocks before copying anything, so it is the
    // one that gets EINTR.
    bool parked = true;
    bool interrupted = true;
    for (int round = 0; round < 2; ++round) {
        parked = parked && waitUntilParkedIn(tid.load(), {SYS_sendto});
        interrupted = interrupted && parked && interrupt(sendThread);
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }

    std::string received;
    std::array<char, 65536> buf{};
    while (received.size() < payload.size()) {
        std::size_t const got = serverSide.recvSome(buf.data(), buf.size());
        if (got == 0) {
            break;
        }
        received.append(buf.data(), got);
    }
    sendThread.join();

    REQUIRE(parked);
    REQUIRE(interrupted);
    REQUIRE(failure == nullptr);
    CHECK(received == payload);
}

#endif  // __linux__

// Mutation: drop the ECONNRESET arm from TcpSocket::recvSome (an abortive close throws instead of reading as a close).
TEST_CASE("TcpSocket::recvSome: a peer's abortive close reads as an orderly one", "[net][tcp]") {
    auto listener = TcpSocket::listen(0);
    TcpSocket serverSide;
    std::thread acceptThread{[&] { serverSide = listener.accept(); }};
    {
        auto const clientSide = TcpSocket::connect("127.0.0.1", listener.boundPort(), std::chrono::milliseconds{2000});
        acceptThread.join();
        linger abortive{};
        abortive.l_onoff = 1;
        abortive.l_linger = 0;
        REQUIRE(::setsockopt(clientSide.nativeHandle(), SOL_SOCKET, SO_LINGER, &abortive, sizeof(abortive)) == 0);
        // clientSide destructs here: an RST, not a FIN.
    }
    std::array<char, 16> buf{};
    CHECK(serverSide.recvSome(buf.data(), buf.size()) == 0U);
}

// Mutation: make setRecvTimeout set SO_SNDTIMEO (the receive is unbounded, and only the backstop's shutdown ends
// it, with 0 instead of a throw).
TEST_CASE("TcpSocket::setRecvTimeout bounds a receive from a peer that never writes", "[net][tcp]") {
    auto listener = TcpSocket::listen(0);
    TcpSocket serverSide;
    std::thread acceptThread{[&] { serverSide = listener.accept(); }};
    auto const clientSide = TcpSocket::connect("127.0.0.1", listener.boundPort(), std::chrono::milliseconds{2000});
    acceptThread.join();

    REQUIRE(serverSide.setRecvTimeout(std::chrono::milliseconds{1100}));
    timeval applied{};
    socklen_t appliedLen = sizeof(applied);
    REQUIRE(::getsockopt(serverSide.nativeHandle(), SOL_SOCKET, SO_RCVTIMEO, &applied, &appliedLen) == 0);
    CHECK(applied.tv_sec == 1);
    CHECK(applied.tv_usec >= 99'000);  // the kernel rounds to its tick
    CHECK(applied.tv_usec <= 101'000);

    std::atomic<bool> done{false};
    std::thread backstop{[&] {
        for (int i = 0; i < 1000 && !done.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
        serverSide.shutdownBoth();
    }};
    std::array<char, 16> buf{};
    bool threw = false;
    try {
        static_cast<void>(serverSide.recvSome(buf.data(), buf.size()));
    } catch (const std::runtime_error&) {
        threw = true;
    }
    done = true;
    backstop.join();
    CHECK(threw);
}

// Mutation: return std::nullopt from tryAccept for every errno (an accept on a socket that is not listening reads as
// "nothing pending" instead of failing).
TEST_CASE("TcpSocket::tryAccept: throws on a socket that is not listening", "[net][tcp]") {
    auto listener = TcpSocket::listen(0);
    TcpSocket serverSide;
    std::thread acceptThread{[&] { serverSide = listener.accept(); }};
    auto clientSide = TcpSocket::connect("127.0.0.1", listener.boundPort(), std::chrono::milliseconds{2000});
    acceptThread.join();

    // A connected socket: accept(2) fails with EINVAL, not EAGAIN.
    REQUIRE_THROWS_WITH(clientSide.tryAccept(), Catch::Matchers::ContainsSubstring("TcpSocket::tryAccept"));
}

// Mutation: drop the `flags >= 0` check from setNonBlocking (an F_GETFL failure of -1 is OR-ed with O_NONBLOCK and
// still reported through the F_SETFL result alone).
TEST_CASE("TcpSocket::setNonBlocking reports failure for a descriptor fcntl rejects", "[net][tcp]") {
    // Far above any RLIMIT_NOFILE, so no descriptor of this number can exist and
    // nothing else in the process can be touched by mistake.
    TcpSocket bogus{std::numeric_limits<int>::max() - 1};
    bool const applied = bogus.setNonBlocking();
    static_cast<void>(bogus.release());
    CHECK_FALSE(applied);
}

// Mutation: treat an immediate connect() failure other than EINPROGRESS as success (a socket is returned for an
// address TCP cannot reach).
TEST_CASE("TcpSocket::connect: an address connect() refuses at once fails cleanly", "[net][tcp]") {
    // TCP to a multicast address fails inside connect() itself, with ENETUNREACH,
    // whatever the routing table holds: there is no handshake to wait for.
    REQUIRE_THROWS_WITH(TcpSocket::connect("224.0.0.1", 80, std::chrono::milliseconds{2000}),
                        Catch::Matchers::ContainsSubstring("could not connect"));
}

// SPDX-License-Identifier: Apache-2.0

#include <fcntl.h>
#include <poll.h>
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

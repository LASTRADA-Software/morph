// SPDX-License-Identifier: Apache-2.0

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

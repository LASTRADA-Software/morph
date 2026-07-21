// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstring>
#include <morph/net/detail/tcp_socket.hpp>
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

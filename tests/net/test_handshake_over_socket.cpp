// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <chrono>
#include <exception>
#include <morph/net/detail/tcp_socket.hpp>
#include <morph/net/detail/ws_handshake.hpp>
#include <stdexcept>
#include <string>
#include <thread>

using morph::net::detail::ParsedWsUrl;
using morph::net::detail::TcpSocket;

TEST_CASE("performClientHandshake/performServerHandshake complete over a real socket", "[net][handshake][socket]") {
    auto listener = TcpSocket::listen(0);
    std::uint16_t const port = listener.boundPort();

    TcpSocket serverSide;
    std::string serverLeftover;
    std::thread serverThread{[&] {
        serverSide = listener.accept();
        serverLeftover = morph::net::detail::performServerHandshake(serverSide);
    }};

    auto clientSide = TcpSocket::connect("127.0.0.1", port, std::chrono::milliseconds{2000});
    ParsedWsUrl url{"127.0.0.1", port, "/"};
    std::string clientLeftover = morph::net::detail::performClientHandshake(clientSide, url);
    serverThread.join();

    REQUIRE(clientLeftover.empty());
    REQUIRE(serverLeftover.empty());
}

TEST_CASE("readHttpHeaderBlock (via performServerHandshake) returns leftover bytes sent right after the handshake",
          "[net][handshake][socket]") {
    auto listener = TcpSocket::listen(0);
    std::uint16_t const port = listener.boundPort();

    TcpSocket serverSide;
    std::string serverLeftover;
    std::thread serverThread{[&] {
        serverSide = listener.accept();
        serverLeftover = morph::net::detail::performServerHandshake(serverSide);
    }};

    auto clientSide = TcpSocket::connect("127.0.0.1", port, std::chrono::milliseconds{2000});
    std::string key = morph::net::detail::generateClientKey();
    ParsedWsUrl url{"127.0.0.1", port, "/"};
    std::string request = morph::net::detail::buildClientHandshakeRequest(url, key);
    // Pipeline the request together with extra bytes so the server's header
    // read has to hand back leftover bytes (simulating a client that sends
    // its first WS frame immediately after the handshake request, in the
    // same TCP segment).
    std::string pipelined = request + "EXTRA-BYTES-AFTER-HANDSHAKE";
    clientSide.sendAll(pipelined.data(), pipelined.size());

    serverThread.join();
    REQUIRE(serverLeftover == "EXTRA-BYTES-AFTER-HANDSHAKE");
}

TEST_CASE("performClientHandshake throws when the server closes before responding", "[net][handshake][socket]") {
    auto listener = TcpSocket::listen(0);
    std::uint16_t const port = listener.boundPort();

    std::thread serverThread{[&] {
        auto serverSide = listener.accept();
        // Accept, then immediately let serverSide go out of scope (closing
        // it) without completing the handshake.
    }};

    auto clientSide = TcpSocket::connect("127.0.0.1", port, std::chrono::milliseconds{2000});
    ParsedWsUrl url{"127.0.0.1", port, "/"};
    serverThread.join();
    REQUIRE_THROWS_AS(morph::net::detail::performClientHandshake(clientSide, url), std::runtime_error);
}

// readHttpHeaderBlock, ws_handshake.hpp:282-284 — the 64 KiB header safety
// cap. This needs a real socket (unlike ws_handshake.hpp's other 14
// string-parsing findings): the cap is only checked between reads, so it
// takes an actual peer sending header-shaped bytes that never complete the
// "\r\n\r\n" terminator.
TEST_CASE("readHttpHeaderBlock throws once the header exceeds the 64 KiB safety cap", "[net][handshake][socket]") {
    auto listener = TcpSocket::listen(0);
    std::uint16_t const port = listener.boundPort();

    TcpSocket serverSide;
    std::exception_ptr serverException;
    std::thread serverThread{[&] {
        serverSide = listener.accept();
        try {
            static_cast<void>(morph::net::detail::readHttpHeaderBlock(serverSide));
        } catch (...) {
            serverException = std::current_exception();
        }
    }};

    auto clientSide = TcpSocket::connect("127.0.0.1", port, std::chrono::milliseconds{2000});
    // Comfortably more than 64 KiB, and more than the worst-case amount
    // readHttpHeaderBlock could have already consumed by the time its cap
    // check trips (64 KiB rounded up to the next 4 KiB recv chunk) — so the
    // cap fires from data the client has already fully queued, rather than
    // the server blocking on recvSome() for bytes that will never arrive.
    std::string oversized(96 * 1024, 'A');
    clientSide.sendAll(oversized.data(), oversized.size());

    serverThread.join();
    REQUIRE(serverException != nullptr);
    REQUIRE_THROWS_WITH(std::rethrow_exception(serverException), Catch::Matchers::ContainsSubstring("64 KiB"));
}

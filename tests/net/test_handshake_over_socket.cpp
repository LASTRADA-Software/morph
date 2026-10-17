// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <chrono>
#include <cstddef>
#include <exception>
#include <morph/net/detail/tcp_socket.hpp>
#include <morph/net/detail/ws_handshake.hpp>
#include <stdexcept>
#include <string>
#include <thread>

using morph::net::detail::ParsedWsUrl;
using morph::net::detail::TcpSocket;

// ── The four blocking accept()s here carry no deadline, and why ─────────────
//
// Every one of them runs on the server thread this file spawns, while the
// connection that satisfies it is made by the *main* thread immediately after,
// under `TcpSocket::connect()`'s own 2s deadline -- which throws rather than
// returning on expiry. The main thread only ever reaches `serverThread.join()`
// with that connection already established against this listener: loopback,
// a 64-deep backlog, one consumer.
//
// That is the opposite shape from the one site in these tests that does need a
// bound (`FakeWsServer::acceptAndHandshake()`, a *main-thread* `accept()` waiting on
// the I/O loop of the component under test), and it is the reason a deadline
// here would be a path nothing can take. An untakeable timeout path is worse
// than none: it reads as a hazard that was found and handled.
//
// The long form of the argument, the mutation that shows the `connect()` is
// the load-bearing step, and the ctest `TIMEOUT 120` backstop if it is ever
// wrong, are in the note above the round-trip test in test_tcp_socket.cpp.
// Verification status is the same as there: inferred from reading plus
// `tcp_socket.hpp`'s `accept()` contract, not measured on these four sites.
//
// A `connect()` that throws aborts via `~std::thread` instead of hanging;
// the error it discards on the way is reported nowhere.

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
    std::string oversized(std::size_t{96} * 1024, 'A');
    clientSide.sendAll(oversized.data(), oversized.size());

    serverThread.join();
    REQUIRE(serverException != nullptr);
    REQUIRE_THROWS_WITH(std::rethrow_exception(serverException), Catch::Matchers::ContainsSubstring("64 KiB"));
}

// Mutation: skip waitReadableUntil when bounded (the read still succeeds, but the timeout case below blocks forever).
TEST_CASE("readHttpHeaderBlock with a timeout returns a header that arrives within it", "[net][handshake][socket]") {
    auto listener = TcpSocket::listen(0);
    std::uint16_t const port = listener.boundPort();

    TcpSocket serverSide;
    morph::net::detail::HandshakeReadResult result;
    std::exception_ptr serverException;
    std::thread serverThread{[&] {
        serverSide = listener.accept();
        try {
            result = morph::net::detail::readHttpHeaderBlock(serverSide, std::chrono::seconds{20});
        } catch (...) {
            serverException = std::current_exception();
        }
    }};

    auto clientSide = TcpSocket::connect("127.0.0.1", port, std::chrono::milliseconds{2000});
    std::string const request = "GET / HTTP/1.1\r\nHost: x\r\n\r\ntail";
    clientSide.sendAll(request.data(), request.size());

    serverThread.join();
    REQUIRE(serverException == nullptr);
    CHECK(result.header == "GET / HTTP/1.1\r\nHost: x");
    CHECK(result.leftover == "tail");
}

// Mutation: drop the kTimedOut throw from waitReadableUntil (recvSome then blocks on a peer that never writes, and
// the read only ends when the client closes -- with "peer closed", not "timed out").
TEST_CASE("readHttpHeaderBlock with a timeout throws once a silent peer lets it pass", "[net][handshake][socket]") {
    auto listener = TcpSocket::listen(0);
    std::uint16_t const port = listener.boundPort();

    TcpSocket serverSide;
    std::exception_ptr serverException;
    std::thread serverThread{[&] {
        serverSide = listener.accept();
        try {
            static_cast<void>(morph::net::detail::readHttpHeaderBlock(serverSide, std::chrono::milliseconds{100}));
        } catch (...) {
            serverException = std::current_exception();
        }
    }};

    // Connected and silent: sends a partial header and never the terminator.
    auto clientSide = TcpSocket::connect("127.0.0.1", port, std::chrono::milliseconds{2000});
    std::string const partial = "GET / HTTP/1.1\r\n";
    clientSide.sendAll(partial.data(), partial.size());

    // A read that ignored its deadline would block until the peer goes away:
    // ending the connection after 10s turns that into a wrong message rather
    // than a hung test.
    std::atomic<bool> serverDone{false};
    std::thread backstop{[&] {
        for (int i = 0; i < 1000 && !serverDone.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
        clientSide.shutdownBoth();
    }};
    serverThread.join();
    serverDone = true;
    backstop.join();
    REQUIRE(serverException != nullptr);
    REQUIRE_THROWS_WITH(std::rethrow_exception(serverException), Catch::Matchers::ContainsSubstring("timed out"));
}

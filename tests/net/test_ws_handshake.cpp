// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <morph/net/detail/ws_handshake.hpp>
#include <stdexcept>
#include <string>

// ── parseWsUrl ────────────────────────────────────────────────────────────

TEST_CASE("parseWsUrl splits host/port/path", "[net][handshake][url]") {
    auto url = morph::net::detail::parseWsUrl("ws://127.0.0.1:9001/chat");
    REQUIRE(url.host == "127.0.0.1");
    REQUIRE(url.port == 9001U);
    REQUIRE(url.path == "/chat");
}

TEST_CASE("parseWsUrl defaults to / when no path is given", "[net][handshake][url]") {
    auto url = morph::net::detail::parseWsUrl("ws://example.com:8080");
    REQUIRE(url.host == "example.com");
    REQUIRE(url.port == 8080U);
    REQUIRE(url.path == "/");
}

TEST_CASE("parseWsUrl rejects wss:// with a clear message", "[net][handshake][url]") {
    REQUIRE_THROWS_WITH(morph::net::detail::parseWsUrl("wss://127.0.0.1:9001"),
                        Catch::Matchers::ContainsSubstring("wss://"));
}

TEST_CASE("parseWsUrl rejects a URL with no port", "[net][handshake][url]") {
    REQUIRE_THROWS_AS(morph::net::detail::parseWsUrl("ws://127.0.0.1"), std::runtime_error);
}

TEST_CASE("parseWsUrl rejects a non-ws scheme", "[net][handshake][url]") {
    REQUIRE_THROWS_AS(morph::net::detail::parseWsUrl("http://127.0.0.1:80"), std::runtime_error);
}

// ── computeAcceptKey — RFC 6455 §1.3 worked example ─────────────────────────

TEST_CASE("computeAcceptKey matches the RFC 6455 worked example", "[net][handshake][accept]") {
    REQUIRE(morph::net::detail::computeAcceptKey("dGhlIHNhbXBsZSBub25jZQ==") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

// ── generateClientKey ────────────────────────────────────────────────────

TEST_CASE("generateClientKey produces a 24-character base64 string (16 raw bytes)", "[net][handshake][key]") {
    auto key = morph::net::detail::generateClientKey();
    REQUIRE(key.size() == 24U);
    REQUIRE(key != morph::net::detail::generateClientKey());  // vanishingly unlikely to collide
}

// ── Client request / server response round trip ─────────────────────────

TEST_CASE("buildClientHandshakeRequest / parseClientHandshakeRequest round-trip", "[net][handshake][roundtrip]") {
    morph::net::detail::ParsedWsUrl url{"127.0.0.1", 9001, "/chat"};
    std::string key = "dGhlIHNhbXBsZSBub25jZQ==";
    std::string request = morph::net::detail::buildClientHandshakeRequest(url, key);

    REQUIRE(request.find("GET /chat HTTP/1.1\r\n") == 0U);
    REQUIRE(request.find("Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n") != std::string::npos);
    REQUIRE(request.find("\r\n\r\n") == request.size() - 4);

    // Strip the trailing blank-line terminator before parsing (that's the
    // handshake-reader's job in Task 6 — here we exercise the parser alone).
    std::string headerBlock = request.substr(0, request.size() - 4);
    auto parsed = morph::net::detail::parseClientHandshakeRequest(headerBlock);
    REQUIRE(parsed.key == key);
    REQUIRE(parsed.path == "/chat");
}

TEST_CASE("buildServerHandshakeResponse / verifyServerHandshakeResponse round-trip", "[net][handshake][roundtrip]") {
    std::string key = morph::net::detail::generateClientKey();
    std::string response = morph::net::detail::buildServerHandshakeResponse(key);
    REQUIRE(response.find("HTTP/1.1 101 Switching Protocols\r\n") == 0U);

    std::string headerBlock = response.substr(0, response.size() - 4);
    // Must not throw.
    morph::net::detail::verifyServerHandshakeResponse(headerBlock, key);
}

TEST_CASE("verifyServerHandshakeResponse rejects a mismatched accept key", "[net][handshake][roundtrip]") {
    std::string response = morph::net::detail::buildServerHandshakeResponse("dGhlIHNhbXBsZSBub25jZQ==");
    std::string headerBlock = response.substr(0, response.size() - 4);
    REQUIRE_THROWS_AS(morph::net::detail::verifyServerHandshakeResponse(headerBlock, "a-different-key"),
                      std::runtime_error);
}

TEST_CASE("parseClientHandshakeRequest rejects a request with no Sec-WebSocket-Key", "[net][handshake][roundtrip]") {
    std::string headerBlock = "GET / HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade";
    REQUIRE_THROWS_AS(morph::net::detail::parseClientHandshakeRequest(headerBlock), std::runtime_error);
}

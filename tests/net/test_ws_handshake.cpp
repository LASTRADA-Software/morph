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

// parseWsUrl, ws_handshake.hpp:74 — colonPos == 0 (an empty host before the
// port colon), distinct from colonPos == npos (no colon at all, covered above).
TEST_CASE("parseWsUrl rejects an empty host before the port colon", "[net][handshake][url]") {
    REQUIRE_THROWS_WITH(morph::net::detail::parseWsUrl("ws://:1234"),
                        Catch::Matchers::ContainsSubstring("explicit host and port"));
}

// parseWsUrl, ws_handshake.hpp:79-81 — empty port string (colon present, nothing after it).
TEST_CASE("parseWsUrl rejects an empty port string", "[net][handshake][url]") {
    REQUIRE_THROWS_WITH(morph::net::detail::parseWsUrl("ws://host:"),
                        Catch::Matchers::ContainsSubstring("invalid port"));
}

// parseWsUrl, ws_handshake.hpp:84-86 — a non-numeric character in the port.
// `abc` only exercises the `ch > '9'` side of `ch < '0' || ch > '9'` (every
// letter's ASCII value is above '9'); the second case below exercises the
// `ch < '0'` side directly, closing the last branch arm this file's own
// re-measurement turned up after the audit's 15 findings were closed.
TEST_CASE("parseWsUrl rejects a non-numeric port", "[net][handshake][url]") {
    REQUIRE_THROWS_WITH(morph::net::detail::parseWsUrl("ws://host:abc"),
                        Catch::Matchers::ContainsSubstring("invalid port"));
}

TEST_CASE("parseWsUrl rejects a port containing a character below '0' (e.g. '.')", "[net][handshake][url]") {
    REQUIRE_THROWS_WITH(morph::net::detail::parseWsUrl("ws://host:1.5"),
                        Catch::Matchers::ContainsSubstring("invalid port"));
}

// parseWsUrl, ws_handshake.hpp:88-90 — port value exceeds 65535 mid-parse.
TEST_CASE("parseWsUrl rejects a port above 65535", "[net][handshake][url]") {
    REQUIRE_THROWS_WITH(morph::net::detail::parseWsUrl("ws://host:99999"),
                        Catch::Matchers::ContainsSubstring("port out of range"));
}

// parseWsUrl, ws_handshake.hpp:92-94 — an all-numeric port that parses to 0.
TEST_CASE("parseWsUrl rejects a port of 0", "[net][handshake][url]") {
    REQUIRE_THROWS_WITH(morph::net::detail::parseWsUrl("ws://host:0"),
                        Catch::Matchers::ContainsSubstring("invalid port"));
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

// ── ws_handshake_impl::splitLines — while-condition's natural-false exit ────
//
// Task 5's audit classified ws_handshake.hpp:144's `while (start <
// raw.size())` false-exit as "(b) unreachable by construction", reasoning
// that splitLines' only two callers (parseClientHandshakeRequest,
// verifyServerHandshakeResponse) always receive a header block whose
// trailing "\r\n\r\n" was already stripped by readHttpHeaderBlock — and
// indeed, `buf.substr(0, pos)` there can never leave the *production*
// header block ending in "\r\n" (if it did, that trailing "\r\n" plus the
// terminator's leading "\r\n" would itself have matched an earlier
// "\r\n\r\n", contradicting `pos` being the first match).
//
// That reasoning only covers callers that go through readHttpHeaderBlock.
// parseClientHandshakeRequest and verifyServerHandshakeResponse are public
// and take an arbitrary string_view — the existing tests in this file
// already call them directly with hand-built strings, bypassing
// readHttpHeaderBlock entirely. Feeding either one a header block that
// itself ends in "\r\n" reaches the loop's natural false-exit directly, so
// this is a genuine (reachable, untested) gap, not dead code — a correction
// to the audit's classification, not a confirmation of it.
TEST_CASE(
    "parseClientHandshakeRequest with a header block ending in \\r\\n reaches "
    "splitLines' natural loop-exit (correcting task-5's 'unreachable' call)",
    "[net][handshake][splitlines]") {
    // "GET / HTTP/1.1\r\n" ends exactly on a "\r\n" boundary: after splitLines
    // consumes the line, `start` lands exactly on raw.size(), so `while
    // (start < raw.size())` evaluates to false on its own — not via the
    // internal "no more \r\n" break used by every other test in this file.
    REQUIRE_THROWS_WITH(morph::net::detail::parseClientHandshakeRequest("GET / HTTP/1.1\r\n"),
                        Catch::Matchers::ContainsSubstring("Sec-WebSocket-Key"));
}

// ── ws_handshake_impl::trimLeadingSpace — empty/all-space input ─────────────

// trimLeadingSpace, ws_handshake.hpp:158 — the loop's natural false-exit,
// hit both immediately (an empty header value) and after consuming every
// character (a header value that is entirely spaces).
TEST_CASE("parseClientHandshakeRequest accepts header lines with an empty or all-space value",
          "[net][handshake][trim]") {
    std::string headerBlock =
        "GET / HTTP/1.1\r\nSec-WebSocket-Key: abc\r\nUpgrade: websocket\r\nX-Empty:\r\nX-Spaces:    ";
    auto parsed = morph::net::detail::parseClientHandshakeRequest(headerBlock);
    REQUIRE(parsed.key == "abc");
    REQUIRE(parsed.path == "/");
}

// ── parseClientHandshakeRequest — remaining string-parsing gaps ─────────────

// parseClientHandshakeRequest, ws_handshake.hpp:183-185 — empty header block.
TEST_CASE("parseClientHandshakeRequest rejects an empty header block", "[net][handshake][roundtrip]") {
    REQUIRE_THROWS_WITH(morph::net::detail::parseClientHandshakeRequest(""),
                        Catch::Matchers::ContainsSubstring("empty request"));
}

// parseClientHandshakeRequest, ws_handshake.hpp:187-189 — non-GET method.
TEST_CASE("parseClientHandshakeRequest rejects a non-GET method", "[net][handshake][roundtrip]") {
    std::string headerBlock = "POST / HTTP/1.1\r\nSec-WebSocket-Key: abc\r\nUpgrade: websocket";
    REQUIRE_THROWS_WITH(morph::net::detail::parseClientHandshakeRequest(headerBlock),
                        Catch::Matchers::ContainsSubstring("expected a GET request line"));
}

// parseClientHandshakeRequest, ws_handshake.hpp:192-194 — request line with
// no space terminating the path.
TEST_CASE("parseClientHandshakeRequest rejects a request line with no path-terminating space",
          "[net][handshake][roundtrip]") {
    REQUIRE_THROWS_WITH(morph::net::detail::parseClientHandshakeRequest("GET /nospace"),
                        Catch::Matchers::ContainsSubstring("malformed request line"));
}

// parseClientHandshakeRequest, ws_handshake.hpp:202-204 — a header line with
// no colon is skipped (`continue`) rather than rejected.
TEST_CASE("parseClientHandshakeRequest skips a header line with no colon", "[net][handshake][roundtrip]") {
    std::string headerBlock = "GET / HTTP/1.1\r\nSec-WebSocket-Key: abc\r\nUpgrade: websocket\r\ngarbage-no-colon";
    auto parsed = morph::net::detail::parseClientHandshakeRequest(headerBlock);
    REQUIRE(parsed.key == "abc");
    REQUIRE(parsed.path == "/");
}

// parseClientHandshakeRequest, ws_handshake.hpp:216-218 — Sec-WebSocket-Key
// present but Upgrade specifically absent (the existing no-key test's
// fixture always includes Upgrade, so this combination was untested).
TEST_CASE("parseClientHandshakeRequest rejects a request with no Upgrade header", "[net][handshake][roundtrip]") {
    std::string headerBlock = "GET / HTTP/1.1\r\nSec-WebSocket-Key: abc";
    REQUIRE_THROWS_WITH(morph::net::detail::parseClientHandshakeRequest(headerBlock),
                        Catch::Matchers::ContainsSubstring("missing Upgrade header"));
}

// ── verifyServerHandshakeResponse — remaining string-parsing gaps ───────────

// verifyServerHandshakeResponse, ws_handshake.hpp:231-233 — empty response.
TEST_CASE("verifyServerHandshakeResponse rejects an empty response", "[net][handshake][roundtrip]") {
    REQUIRE_THROWS_WITH(morph::net::detail::verifyServerHandshakeResponse("", "any-key"),
                        Catch::Matchers::ContainsSubstring("empty response"));
}

// verifyServerHandshakeResponse, ws_handshake.hpp:235-238 — a non-101 status
// line (the existing mismatched-accept-key test still has a 101 status; only
// the accept value differs there).
TEST_CASE("verifyServerHandshakeResponse rejects a non-101 status line", "[net][handshake][roundtrip]") {
    std::string headerBlock = "HTTP/1.1 400 Bad Request\r\nUpgrade: websocket";
    REQUIRE_THROWS_WITH(morph::net::detail::verifyServerHandshakeResponse(headerBlock, "any-key"),
                        Catch::Matchers::ContainsSubstring("did not return 101"));
}

// verifyServerHandshakeResponse, ws_handshake.hpp:243-245 — a header line
// with no colon is skipped, mirroring parseClientHandshakeRequest's
// analogous check (finding #11 above, lines 202-204) but in the *response*
// parser's own header loop. Task 5's findings doc does not list this one —
// its own baseline's 27 missed lines include both instances of this
// skip-on-no-colon branch (203-204 and 244-245), but only the request-side
// one got an explicit, numbered finding. Adding it here as a 16th genuine
// gap discovered while verifying the audit, not just transcribing it.
TEST_CASE(
    "verifyServerHandshakeResponse skips a header line with no colon "
    "(undocumented 16th gap: the audit's own 27-missed-line baseline "
    "includes this line pair, but no finding named it)",
    "[net][handshake][roundtrip]") {
    std::string key = morph::net::detail::generateClientKey();
    std::string accept = morph::net::detail::computeAcceptKey(key);
    std::string headerBlock =
        "HTTP/1.1 101 Switching Protocols\r\ngarbage-no-colon\r\nSec-WebSocket-Accept: " + accept;
    // Must not throw: the colon-less line is skipped, and the accept key
    // still matches.
    morph::net::detail::verifyServerHandshakeResponse(headerBlock, key);
}

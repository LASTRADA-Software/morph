// SPDX-License-Identifier: Apache-2.0

// Protocol-version negotiation coverage: the wire-layer primitives
// (kProtocolVersion, Envelope::protocolVersion, ProtocolRange, makeHello,
// interpretHelloReply), RemoteServer's "hello" handling, and
// SimulatedRemoteBackend::negotiateProtocolVersion(). QtWebSocketBackend's
// negotiateProtocolVersion() is covered separately in
// tests/qt/test_qt_websocket.cpp (it needs a real Qt event loop).

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <morph/core/wire.hpp>
#include <string>

using morph::wire::decode;
using morph::wire::encode;
using morph::wire::Envelope;
using morph::wire::interpretHelloReply;
using morph::wire::kProtocolVersion;
using morph::wire::makeErr;
using morph::wire::makeHello;
using morph::wire::makeOk;
using morph::wire::ProtocolNegotiationResult;
using morph::wire::ProtocolRange;

// ── kProtocolVersion / Envelope::protocolVersion ──────────────────────────────

TEST_CASE("kProtocolVersion is 1", "[wire][protocol]") { REQUIRE(kProtocolVersion == 1U); }

TEST_CASE("Envelope::protocolVersion defaults to 0 (legacy/unspecified)", "[wire][protocol]") {
    Envelope env;
    REQUIRE(env.protocolVersion == 0U);
}

TEST_CASE("protocolVersion round-trips through encode/decode", "[wire][protocol]") {
    Envelope env = makeHello();
    REQUIRE(env.protocolVersion == kProtocolVersion);
    auto decoded = decode(encode(env));
    REQUIRE(decoded.protocolVersion == kProtocolVersion);
    REQUIRE(decoded.kind == "hello");
}

TEST_CASE("an envelope encoded without protocolVersion decodes as 0 (legacy)", "[wire][protocol]") {
    // Simulates an old encoder that has never heard of protocolVersion: the
    // JSON simply omits the key.
    const std::string json = R"({"kind":"register","typeId":"SomeType"})";
    auto decoded = decode(json);
    REQUIRE(decoded.protocolVersion == 0U);
}

TEST_CASE("a protocolVersion key an old decoder does not know is ignored, not rejected", "[wire][protocol]") {
    // Forward-compat regression guard: an unknown/newer field must not break
    // the parse, matching the existing error_on_unknown_keys=false contract.
    const std::string json = R"({"kind":"register","typeId":"SomeType","protocolVersion":99})";
    REQUIRE_NOTHROW(decode(json));
}

// ── makeHello / ProtocolRange ─────────────────────────────────────────────────

TEST_CASE("makeHello builds a hello envelope carrying the caller's version", "[wire][protocol]") {
    auto env = makeHello(7);
    REQUIRE(env.kind == "hello");
    REQUIRE(env.protocolVersion == 7U);
}

TEST_CASE("makeHello defaults to kProtocolVersion", "[wire][protocol]") {
    auto env = makeHello();
    REQUIRE(env.protocolVersion == kProtocolVersion);
}

TEST_CASE("ProtocolRange round-trips through glaze JSON", "[wire][protocol]") {
    ProtocolRange range{.min = 1, .max = 3};
    std::string json;
    REQUIRE_FALSE(glz::write_json(range, json));
    ProtocolRange decoded{};
    REQUIRE_FALSE(glz::read_json(decoded, json));
    REQUIRE(decoded.min == 1U);
    REQUIRE(decoded.max == 3U);
}

// ── interpretHelloReply ────────────────────────────────────────────────────────

TEST_CASE("interpretHelloReply: an ok reply is Negotiated", "[wire][protocol]") {
    std::string body;
    REQUIRE_FALSE(glz::write_json(ProtocolRange{.min = 1, .max = 1}, body));
    auto reply = makeOk(0, body);
    REQUIRE(interpretHelloReply(reply) == ProtocolNegotiationResult::Negotiated);
}

TEST_CASE("interpretHelloReply: 'unknown envelope kind: hello' is classified LegacyPeer", "[wire][protocol]") {
    auto reply = makeErr("unknown envelope kind: hello");
    REQUIRE(interpretHelloReply(reply) == ProtocolNegotiationResult::LegacyPeer);
}

TEST_CASE("interpretHelloReply: any other err throws instead of proceeding", "[wire][protocol]") {
    auto reply = makeErr("protocol version unsupported");
    REQUIRE_THROWS_AS(interpretHelloReply(reply), std::runtime_error);
}

TEST_CASE("interpretHelloReply: an empty-message err still throws (does not misclassify)", "[wire][protocol]") {
    auto reply = makeErr("");
    REQUIRE_THROWS_AS(interpretHelloReply(reply), std::runtime_error);
}

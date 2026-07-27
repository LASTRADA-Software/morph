// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <morph/net/detail/ws_frame.hpp>
#include <stdexcept>
#include <string>

using morph::net::detail::encodeWsFrame;
using morph::net::detail::WsFrame;
using morph::net::detail::WsFrameReader;
using morph::net::detail::WsOpcode;

TEST_CASE("encodeWsFrame/WsFrameReader round-trip a small masked text frame", "[net][frame]") {
    std::string payload = "hello";
    std::string wire = encodeWsFrame(WsOpcode::kText, payload, /*mask=*/true);

    WsFrameReader reader;
    reader.feed(wire);
    auto frame = reader.tryExtractFrame();
    REQUIRE(frame.has_value());
    REQUIRE(frame->opcode == WsOpcode::kText);
    REQUIRE(frame->payload == payload);
}

TEST_CASE("encodeWsFrame/WsFrameReader round-trip an unmasked (server-side) frame", "[net][frame]") {
    std::string payload = R"({"kind":"ok","callId":7})";
    std::string wire = encodeWsFrame(WsOpcode::kText, payload, /*mask=*/false);

    WsFrameReader reader;
    reader.feed(wire);
    auto frame = reader.tryExtractFrame();
    REQUIRE(frame.has_value());
    REQUIRE(frame->payload == payload);
}

TEST_CASE("encodeWsFrame/WsFrameReader round-trip a payload requiring the 16-bit extended length", "[net][frame]") {
    std::string payload(70000, 'x');  // > 125 and > 65535? no: > 125, exercises the 126 marker
    std::string wire = encodeWsFrame(WsOpcode::kText, payload, /*mask=*/true);

    WsFrameReader reader;
    reader.feed(wire);
    auto frame = reader.tryExtractFrame();
    REQUIRE(frame.has_value());
    REQUIRE(frame->payload.size() == payload.size());
    REQUIRE(frame->payload == payload);
}

TEST_CASE("WsFrameReader reassembles a frame delivered across multiple feed() calls", "[net][frame]") {
    std::string payload = "partial delivery";
    std::string wire = encodeWsFrame(WsOpcode::kText, payload, /*mask=*/true);

    WsFrameReader reader;
    reader.feed(wire.substr(0, 3));
    REQUIRE_FALSE(reader.tryExtractFrame().has_value());
    reader.feed(wire.substr(3));
    auto frame = reader.tryExtractFrame();
    REQUIRE(frame.has_value());
    REQUIRE(frame->payload == payload);
}

TEST_CASE("WsFrameReader extracts two frames fed back-to-back in one buffer", "[net][frame]") {
    std::string wireA = encodeWsFrame(WsOpcode::kText, "first", true);
    std::string wireB = encodeWsFrame(WsOpcode::kText, "second", true);

    WsFrameReader reader;
    reader.feed(wireA + wireB);
    auto frameA = reader.tryExtractFrame();
    auto frameB = reader.tryExtractFrame();
    REQUIRE(frameA.has_value());
    REQUIRE(frameB.has_value());
    REQUIRE(frameA->payload == "first");
    REQUIRE(frameB->payload == "second");
    REQUIRE_FALSE(reader.tryExtractFrame().has_value());
}

// ── Fragmented messages (RFC 6455 §5.4) ─────────────────────────────────────
// A peer fragments whenever a message exceeds its outgoing frame size, and
// Qt's QWebSocket defaults that to 512 KiB -- so rejecting fragments broke
// interop with the very transport this project ships, for every payload past
// that size.

namespace {
// encodeWsFrame always sets FIN; rewrite byte 0 to build the fragment shapes a
// real peer emits.
// Extracts a frame that must be there. Returning by value (rather than
// REQUIRE-ing an optional and then dereferencing it) keeps engagement provable
// at every call site — Catch2's REQUIRE is a macro clang-tidy's optional-access
// analysis cannot follow.
WsFrame requireFrame(WsFrameReader& reader) {
    auto frame = reader.tryExtractFrame();
    if (!frame.has_value()) {
        throw std::runtime_error("requireFrame: expected a complete message, got none");
    }
    return std::move(frame).value();
}

std::string fragmentFrame(WsOpcode opcode, const std::string& payload, bool fin) {
    std::string wire = encodeWsFrame(opcode, payload, /*mask=*/true);
    wire.at(0) = static_cast<char>((fin ? 0x80U : 0x00U) | (static_cast<unsigned>(opcode) & 0x0FU));
    return wire;
}
}  // namespace

TEST_CASE("WsFrameReader withholds an incomplete fragmented message", "[net][frame]") {
    WsFrameReader reader;
    reader.feed(fragmentFrame(WsOpcode::kText, "first half ", /*fin=*/false));
    // Not an error, just not a whole message yet.
    REQUIRE_FALSE(reader.tryExtractFrame().has_value());
}

TEST_CASE("WsFrameReader reassembles a fragmented text message", "[net][frame]") {
    WsFrameReader reader;
    reader.feed(fragmentFrame(WsOpcode::kText, "one ", /*fin=*/false));
    reader.feed(fragmentFrame(WsOpcode::kContinuation, "two ", /*fin=*/false));
    reader.feed(fragmentFrame(WsOpcode::kContinuation, "three", /*fin=*/true));

    auto const frame = requireFrame(reader);
    // The completed message carries the opcode of its *first* frame, not the
    // continuation opcode the last one arrived with.
    CHECK(frame.opcode == WsOpcode::kText);
    CHECK(frame.payload == "one two three");
    CHECK_FALSE(reader.tryExtractFrame().has_value());
}

TEST_CASE("WsFrameReader reassembles a message delivered in one buffer", "[net][frame]") {
    WsFrameReader reader;
    std::string wire = fragmentFrame(WsOpcode::kText, "a", /*fin=*/false);
    wire += fragmentFrame(WsOpcode::kContinuation, "b", /*fin=*/false);
    wire += fragmentFrame(WsOpcode::kContinuation, "c", /*fin=*/true);
    wire += encodeWsFrame(WsOpcode::kText, "next", /*mask=*/true);
    reader.feed(wire);

    CHECK(requireFrame(reader).payload == "abc");
    CHECK(requireFrame(reader).payload == "next");
}

TEST_CASE("WsFrameReader passes control frames through mid-reassembly", "[net][frame]") {
    // The RFC explicitly allows a control frame between the fragments of a
    // data message; a ping arriving mid-message must be answerable without
    // corrupting the message being assembled.
    WsFrameReader reader;
    reader.feed(fragmentFrame(WsOpcode::kText, "start ", /*fin=*/false));
    reader.feed(encodeWsFrame(WsOpcode::kPing, "hb", /*mask=*/true));
    reader.feed(fragmentFrame(WsOpcode::kContinuation, "end", /*fin=*/true));

    auto const ping = requireFrame(reader);
    CHECK(ping.opcode == WsOpcode::kPing);
    CHECK(ping.payload == "hb");

    auto const message = requireFrame(reader);
    CHECK(message.opcode == WsOpcode::kText);
    CHECK(message.payload == "start end");
}

TEST_CASE("WsFrameReader rejects a continuation with no message in progress", "[net][frame]") {
    WsFrameReader reader;
    reader.feed(fragmentFrame(WsOpcode::kContinuation, "orphan", /*fin=*/true));
    REQUIRE_THROWS_AS(reader.tryExtractFrame(), std::runtime_error);
}

TEST_CASE("WsFrameReader rejects a new data frame interrupting a fragmented message", "[net][frame]") {
    WsFrameReader reader;
    reader.feed(fragmentFrame(WsOpcode::kText, "half", /*fin=*/false));
    reader.feed(encodeWsFrame(WsOpcode::kText, "interrupting", /*mask=*/true));
    REQUIRE_THROWS_AS(reader.tryExtractFrame(), std::runtime_error);
}

TEST_CASE("WsFrameReader rejects a fragmented control frame", "[net][frame]") {
    WsFrameReader reader;
    reader.feed(fragmentFrame(WsOpcode::kPing, "nope", /*fin=*/false));
    REQUIRE_THROWS_AS(reader.tryExtractFrame(), std::runtime_error);
}

TEST_CASE("WsFrameReader rejects a frame whose declared length exceeds kMaxEnvelopeBytes", "[net][frame]") {
    // Hand-craft a header: FIN=1/opcode=text, MASK=0, len-marker=127 (8-byte
    // extended length) declaring a length one byte over the cap.
    std::string header;
    header.push_back(static_cast<char>(0x81));  // FIN=1, opcode=text
    header.push_back(static_cast<char>(127));   // MASK=0, 8-byte extended length follows
    std::uint64_t const bogusLen = morph::wire::kMaxEnvelopeBytes + 1;
    for (int shift = 56; shift >= 0; shift -= 8) {
        header.push_back(static_cast<char>((bogusLen >> shift) & 0xFFu));
    }
    WsFrameReader reader;
    reader.feed(header);
    REQUIRE_THROWS_AS(reader.tryExtractFrame(), std::runtime_error);
}

TEST_CASE("encodeWsFrame round-trips close/ping/pong opcodes", "[net][frame]") {
    WsFrameReader reader;
    reader.feed(encodeWsFrame(WsOpcode::kClose, "", true));
    auto closeFrame = reader.tryExtractFrame();
    REQUIRE(closeFrame.has_value());
    REQUIRE(closeFrame->opcode == WsOpcode::kClose);

    reader.feed(encodeWsFrame(WsOpcode::kPing, "ping-data", true));
    auto pingFrame = reader.tryExtractFrame();
    REQUIRE(pingFrame.has_value());
    REQUIRE(pingFrame->opcode == WsOpcode::kPing);
    REQUIRE(pingFrame->payload == "ping-data");
}

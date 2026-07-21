// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <morph/net/detail/ws_frame.hpp>
#include <stdexcept>
#include <string>

using morph::net::detail::encodeWsFrame;
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

TEST_CASE("WsFrameReader rejects a fragmented (FIN=0) frame", "[net][frame]") {
    std::string wire = encodeWsFrame(WsOpcode::kText, "oops", true);
    wire[0] = static_cast<char>(static_cast<unsigned char>(wire[0]) & 0x7Fu);  // clear FIN
    WsFrameReader reader;
    reader.feed(wire);
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

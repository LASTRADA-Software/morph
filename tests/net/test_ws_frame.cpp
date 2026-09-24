// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
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

    WsFrameReader reader{/*expectMasked=*/true};
    reader.feed(wire);
    auto frame = reader.tryExtractFrame();
    REQUIRE(frame.has_value());
    REQUIRE(frame->opcode == WsOpcode::kText);
    REQUIRE(frame->payload == payload);
}

TEST_CASE("encodeWsFrame/WsFrameReader round-trip an unmasked (server-side) frame", "[net][frame]") {
    std::string payload = R"({"kind":"ok","callId":7})";
    std::string wire = encodeWsFrame(WsOpcode::kText, payload, /*mask=*/false);

    WsFrameReader reader{/*expectMasked=*/false};
    reader.feed(wire);
    auto frame = reader.tryExtractFrame();
    REQUIRE(frame.has_value());
    REQUIRE(frame->payload == payload);
}

TEST_CASE("encodeWsFrame/WsFrameReader round-trip a payload requiring the 16-bit extended length", "[net][frame]") {
    std::string payload(70000, 'x');  // > 125 and > 65535? no: > 125, exercises the 126 marker
    std::string wire = encodeWsFrame(WsOpcode::kText, payload, /*mask=*/true);

    WsFrameReader reader{/*expectMasked=*/true};
    reader.feed(wire);
    auto frame = reader.tryExtractFrame();
    REQUIRE(frame.has_value());
    REQUIRE(frame->payload.size() == payload.size());
    REQUIRE(frame->payload == payload);
}

TEST_CASE("WsFrameReader withholds a frame whose 16-bit extended length header is incomplete", "[net][frame]") {
    // len-marker 126 (16-bit extended length) with only the 2-byte base
    // header buffered so far: tryExtractRawFrame must return nullopt rather
    // than read _buf[2]/_buf[3] before they have arrived. This payload size
    // (>125, <=65535) also exercises encodeWsFrame's own 126-marker branch,
    // which no other test in this file reaches -- the existing "requiring
    // the 16-bit extended length" test above uses a 70000-byte payload,
    // which is actually too large for the 126 marker and silently falls
    // through to the 64-bit (127) marker instead.
    std::string payload(200, 'y');
    std::string wire = encodeWsFrame(WsOpcode::kText, payload, /*mask=*/false);
    REQUIRE(static_cast<std::uint8_t>(wire[1]) == 126);  // sanity: confirms the 126 marker was used

    WsFrameReader reader{/*expectMasked=*/false};
    reader.feed(wire.substr(0, 2));  // FIN/opcode byte + length-marker byte only
    REQUIRE_FALSE(reader.tryExtractFrame().has_value());

    reader.feed(wire.substr(2));  // the two length bytes, plus payload
    auto frame = reader.tryExtractFrame();
    REQUIRE(frame.has_value());
    REQUIRE(frame->payload == payload);
}

TEST_CASE("WsFrameReader withholds a frame whose 64-bit extended length header is incomplete", "[net][frame]") {
    // len-marker 127 (64-bit extended length) with only the 2-byte base
    // header buffered so far: tryExtractRawFrame must return nullopt rather
    // than read the 8 length bytes at _buf[2..9] before they have arrived.
    std::string payload(70000, 'z');  // > 65535, requires the 127 marker
    std::string wire = encodeWsFrame(WsOpcode::kText, payload, /*mask=*/false);
    REQUIRE(static_cast<std::uint8_t>(wire[1]) == 127);  // sanity: confirms the 127 marker was used

    WsFrameReader reader{/*expectMasked=*/false};
    reader.feed(wire.substr(0, 2));
    REQUIRE_FALSE(reader.tryExtractFrame().has_value());

    reader.feed(wire.substr(2));
    auto frame = reader.tryExtractFrame();
    REQUIRE(frame.has_value());
    REQUIRE(frame->payload == payload);
}

TEST_CASE("WsFrameReader reassembles a frame delivered across multiple feed() calls", "[net][frame]") {
    std::string payload = "partial delivery";
    std::string wire = encodeWsFrame(WsOpcode::kText, payload, /*mask=*/true);

    WsFrameReader reader{/*expectMasked=*/true};
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

    WsFrameReader reader{/*expectMasked=*/true};
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
    WsFrameReader reader{/*expectMasked=*/true};
    reader.feed(fragmentFrame(WsOpcode::kText, "first half ", /*fin=*/false));
    // Not an error, just not a whole message yet.
    REQUIRE_FALSE(reader.tryExtractFrame().has_value());
}

TEST_CASE("WsFrameReader reassembles a fragmented text message", "[net][frame]") {
    WsFrameReader reader{/*expectMasked=*/true};
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
    WsFrameReader reader{/*expectMasked=*/true};
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
    WsFrameReader reader{/*expectMasked=*/true};
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
    WsFrameReader reader{/*expectMasked=*/true};
    reader.feed(fragmentFrame(WsOpcode::kContinuation, "orphan", /*fin=*/true));
    REQUIRE_THROWS_AS(reader.tryExtractFrame(), std::runtime_error);
}

TEST_CASE("WsFrameReader rejects a new data frame interrupting a fragmented message", "[net][frame]") {
    WsFrameReader reader{/*expectMasked=*/true};
    reader.feed(fragmentFrame(WsOpcode::kText, "half", /*fin=*/false));
    reader.feed(encodeWsFrame(WsOpcode::kText, "interrupting", /*mask=*/true));
    REQUIRE_THROWS_AS(reader.tryExtractFrame(), std::runtime_error);
}

TEST_CASE("WsFrameReader rejects a fragmented control frame", "[net][frame]") {
    WsFrameReader reader{/*expectMasked=*/true};
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
    WsFrameReader reader{/*expectMasked=*/false};
    reader.feed(header);
    REQUIRE_THROWS_AS(reader.tryExtractFrame(), std::runtime_error);
}

TEST_CASE("WsFrameReader rejects a reassembled message whose cumulative size exceeds kMaxEnvelopeBytes",
          "[net][frame]") {
    // Distinct from the per-frame cap above: each individual fragment here
    // stays comfortably under kMaxEnvelopeBytes (so tryExtractRawFrame's own
    // check never fires), but appendFragment must still catch the
    // *reassembled total* creeping past the cap -- otherwise a peer could
    // stream unlimited small continuations and grow the assembly buffer
    // without ever tripping a per-frame check.
    constexpr std::size_t chunkSize = std::size_t{3} * 1024 * 1024;  // 3 MiB per fragment
    std::string const chunk(chunkSize, 'x');

    WsFrameReader reader{/*expectMasked=*/true};
    reader.feed(fragmentFrame(WsOpcode::kBinary, chunk, /*fin=*/false));
    REQUIRE_FALSE(reader.tryExtractFrame().has_value());  // 3 MiB so far

    reader.feed(fragmentFrame(WsOpcode::kContinuation, chunk, /*fin=*/false));
    REQUIRE_FALSE(reader.tryExtractFrame().has_value());  // 6 MiB so far, still under the 8 MiB cap

    // Third fragment pushes the cumulative total to 9 MiB, past kMaxEnvelopeBytes (8 MiB).
    reader.feed(fragmentFrame(WsOpcode::kContinuation, chunk, /*fin=*/true));
    REQUIRE_THROWS_AS(reader.tryExtractFrame(), std::runtime_error);
}

TEST_CASE("encodeWsFrame round-trips close/ping/pong opcodes", "[net][frame]") {
    WsFrameReader reader{/*expectMasked=*/true};
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

// ── RFC 6455 conformance: illegal frames a peer must not accept ────────────
// A reader with no role and no length/opcode checks accepts every one of the
// following.

TEST_CASE("WsFrameReader (server role) rejects an unmasked frame from a client", "[net][frame]") {
    WsFrameReader reader{/*expectMasked=*/true};
    reader.feed(encodeWsFrame(WsOpcode::kText, "hi", /*mask=*/false));
    REQUIRE_THROWS_AS(reader.tryExtractFrame(), std::runtime_error);
}

TEST_CASE("WsFrameReader (client role) rejects a masked frame from a server", "[net][frame]") {
    WsFrameReader reader{/*expectMasked=*/false};
    reader.feed(encodeWsFrame(WsOpcode::kText, "hi", /*mask=*/true));
    REQUIRE_THROWS_AS(reader.tryExtractFrame(), std::runtime_error);
}

TEST_CASE("WsFrameReader rejects RSV1 with no extension negotiated", "[net][frame]") {
    std::string wire = encodeWsFrame(WsOpcode::kText, "hi", /*mask=*/true);
    wire[0] = static_cast<char>(static_cast<std::uint8_t>(wire[0]) | 0x40U);
    WsFrameReader reader{/*expectMasked=*/true};
    reader.feed(wire);
    REQUIRE_THROWS_AS(reader.tryExtractFrame(), std::runtime_error);
}

TEST_CASE("WsFrameReader rejects RSV2+RSV3 with no extension negotiated", "[net][frame]") {
    std::string wire = encodeWsFrame(WsOpcode::kText, "hi", /*mask=*/true);
    wire[0] = static_cast<char>(static_cast<std::uint8_t>(wire[0]) | 0x30U);
    WsFrameReader reader{/*expectMasked=*/true};
    reader.feed(wire);
    REQUIRE_THROWS_AS(reader.tryExtractFrame(), std::runtime_error);
}

TEST_CASE("WsFrameReader rejects a reserved non-control opcode", "[net][frame]") {
    std::string wire = encodeWsFrame(WsOpcode::kText, "hi", /*mask=*/true);
    wire[0] = static_cast<char>((static_cast<std::uint8_t>(wire[0]) & 0xF0U) | 0x3U);
    WsFrameReader reader{/*expectMasked=*/true};
    reader.feed(wire);
    REQUIRE_THROWS_AS(reader.tryExtractFrame(), std::runtime_error);
}

TEST_CASE("WsFrameReader rejects a reserved control opcode", "[net][frame]") {
    std::string wire = encodeWsFrame(WsOpcode::kText, "hi", /*mask=*/true);
    wire[0] = static_cast<char>((static_cast<std::uint8_t>(wire[0]) & 0xF0U) | 0xBU);
    WsFrameReader reader{/*expectMasked=*/true};
    reader.feed(wire);
    REQUIRE_THROWS_AS(reader.tryExtractFrame(), std::runtime_error);
}

TEST_CASE("WsFrameReader rejects a Ping payload over 125 bytes", "[net][frame]") {
    std::string const payload(200, 'x');
    WsFrameReader reader{/*expectMasked=*/true};
    reader.feed(encodeWsFrame(WsOpcode::kPing, payload, /*mask=*/true));
    REQUIRE_THROWS_AS(reader.tryExtractFrame(), std::runtime_error);
}

TEST_CASE("WsFrameReader rejects a Close frame with a 1-byte payload", "[net][frame]") {
    WsFrameReader reader{/*expectMasked=*/true};
    reader.feed(encodeWsFrame(WsOpcode::kClose, std::string(1, 'x'), /*mask=*/true));
    REQUIRE_THROWS_AS(reader.tryExtractFrame(), std::runtime_error);
}

TEST_CASE("WsFrameReader rejects Close code 1005 (reserved, must not be sent)", "[net][frame]") {
    std::string payload;
    payload.push_back(static_cast<char>(1005 >> 8));
    payload.push_back(static_cast<char>(1005 & 0xFF));
    WsFrameReader reader{/*expectMasked=*/true};
    reader.feed(encodeWsFrame(WsOpcode::kClose, payload, /*mask=*/true));
    REQUIRE_THROWS_AS(reader.tryExtractFrame(), std::runtime_error);
}

TEST_CASE("WsFrameReader accepts Close code 1000 with a UTF-8 reason", "[net][frame]") {
    std::string payload;
    payload.push_back(static_cast<char>(1000 >> 8));
    payload.push_back(static_cast<char>(1000 & 0xFF));
    payload += "bye";
    WsFrameReader reader{/*expectMasked=*/true};
    reader.feed(encodeWsFrame(WsOpcode::kClose, payload, /*mask=*/true));
    auto frame = reader.tryExtractFrame();
    REQUIRE(frame.has_value());
    REQUIRE(frame->payload == payload);
}

TEST_CASE("WsFrameReader accepts Close code 1008 (the 1007-1011 range)", "[net][frame]") {
    std::string payload;
    payload.push_back(static_cast<char>(1008 >> 8));
    payload.push_back(static_cast<char>(1008 & 0xFF));
    WsFrameReader reader{/*expectMasked=*/true};
    reader.feed(encodeWsFrame(WsOpcode::kClose, payload, /*mask=*/true));
    auto frame = reader.tryExtractFrame();
    REQUIRE(frame.has_value());
}

TEST_CASE("WsFrameReader accepts Close code 1013 (registered after RFC 6455 shipped)", "[net][frame]") {
    // 1012 Service Restart, 1013 Try Again Later and 1014 Bad Gateway were
    // added to the IANA close-code registry after RFC 6455's own §7.4.1 table;
    // a proxy fronting a real server does send them, so rejecting them would
    // drop a legitimate peer.
    std::string payload;
    payload.push_back(static_cast<char>(1013 >> 8));
    payload.push_back(static_cast<char>(1013 & 0xFF));
    WsFrameReader reader{/*expectMasked=*/true};
    reader.feed(encodeWsFrame(WsOpcode::kClose, payload, /*mask=*/true));
    auto frame = reader.tryExtractFrame();
    REQUIRE(frame.has_value());
}

TEST_CASE("WsFrameReader rejects Close code 1015 (reserved, must not be sent)", "[net][frame]") {
    std::string payload;
    payload.push_back(static_cast<char>(1015 >> 8));
    payload.push_back(static_cast<char>(1015 & 0xFF));
    WsFrameReader reader{/*expectMasked=*/true};
    reader.feed(encodeWsFrame(WsOpcode::kClose, payload, /*mask=*/true));
    REQUIRE_THROWS_AS(reader.tryExtractFrame(), std::runtime_error);
}

TEST_CASE("WsFrameReader rejects a Close frame whose reason is not valid UTF-8", "[net][frame]") {
    // RFC 6455 §5.5.1: the bytes after the status code are a reason phrase and
    // must be valid UTF-8, exactly as a Text payload must be.
    std::string payload;
    payload.push_back(static_cast<char>(1000 >> 8));
    payload.push_back(static_cast<char>(1000 & 0xFF));
    payload += "\xC3";  // a 2-byte lead with no continuation byte
    WsFrameReader reader{/*expectMasked=*/true};
    reader.feed(encodeWsFrame(WsOpcode::kClose, payload, /*mask=*/true));
    REQUIRE_THROWS_AS(reader.tryExtractFrame(), std::runtime_error);
}

TEST_CASE("WsFrameReader accepts Close code 3500 (the 3000-4999 range)", "[net][frame]") {
    std::string payload;
    payload.push_back(static_cast<char>(3500 >> 8));
    payload.push_back(static_cast<char>(3500 & 0xFF));
    WsFrameReader reader{/*expectMasked=*/true};
    reader.feed(encodeWsFrame(WsOpcode::kClose, payload, /*mask=*/true));
    auto frame = reader.tryExtractFrame();
    REQUIRE(frame.has_value());
}

TEST_CASE("WsFrameReader rejects Close code 500 (below the valid range)", "[net][frame]") {
    std::string payload;
    payload.push_back(static_cast<char>(500 >> 8));
    payload.push_back(static_cast<char>(500 & 0xFF));
    WsFrameReader reader{/*expectMasked=*/true};
    reader.feed(encodeWsFrame(WsOpcode::kClose, payload, /*mask=*/true));
    REQUIRE_THROWS_AS(reader.tryExtractFrame(), std::runtime_error);
}

TEST_CASE("WsFrameReader rejects Close code 5000 (above the valid range)", "[net][frame]") {
    std::string payload;
    payload.push_back(static_cast<char>(5000 >> 8));
    payload.push_back(static_cast<char>(5000 & 0xFF));
    WsFrameReader reader{/*expectMasked=*/true};
    reader.feed(encodeWsFrame(WsOpcode::kClose, payload, /*mask=*/true));
    REQUIRE_THROWS_AS(reader.tryExtractFrame(), std::runtime_error);
}

TEST_CASE("WsFrameReader rejects a non-minimal 64-bit length encoding", "[net][frame]") {
    // FIN=1/text, unmasked, len-marker=127 (8-byte extended length) declaring
    // a 2-byte payload -- legal encoders never emit this (2 fits the 7-bit
    // field directly, and even the 126 marker would be non-minimal for it).
    std::string header;
    header.push_back(static_cast<char>(0x81));
    header.push_back(static_cast<char>(127));
    for (int i = 0; i < 7; ++i) {
        header.push_back(static_cast<char>(0x00));
    }
    header.push_back(static_cast<char>(0x02));
    header += "hi";
    WsFrameReader reader{/*expectMasked=*/false};
    reader.feed(header);
    REQUIRE_THROWS_AS(reader.tryExtractFrame(), std::runtime_error);
}

TEST_CASE("WsFrameReader accepts a 2-byte UTF-8 sequence", "[net][frame]") {
    std::string const eAcute = "\xC3\xA9";  // U+00E9 "é", 2 bytes
    WsFrameReader reader{/*expectMasked=*/true};
    reader.feed(encodeWsFrame(WsOpcode::kText, eAcute, /*mask=*/true));
    auto frame = reader.tryExtractFrame();
    REQUIRE(frame.has_value());
    REQUIRE(frame->payload == eAcute);
}

TEST_CASE("WsFrameReader accepts a 4-byte UTF-8 sequence", "[net][frame]") {
    std::string const emoji = "\xF0\x9F\x98\x80";  // U+1F600 grinning face, 4 bytes
    WsFrameReader reader{/*expectMasked=*/true};
    reader.feed(encodeWsFrame(WsOpcode::kText, emoji, /*mask=*/true));
    auto frame = reader.tryExtractFrame();
    REQUIRE(frame.has_value());
    REQUIRE(frame->payload == emoji);
}

TEST_CASE("WsFrameReader rejects a 4-byte UTF-8 lead byte beyond the U+10FFFF range", "[net][frame]") {
    WsFrameReader reader{/*expectMasked=*/true};
    reader.feed(encodeWsFrame(WsOpcode::kText, "\xF5\x80\x80\x80", /*mask=*/true));
    REQUIRE_THROWS_AS(reader.tryExtractFrame(), std::runtime_error);
}

TEST_CASE("WsFrameReader rejects a codepoint past U+10FFFF encoded with a valid 4-byte lead", "[net][frame]") {
    // 0xF4 0x90 0x80 0x80 decodes to U+110000 -- one past the Unicode max --
    // even though 0xF4 itself is not rejected by the lead-byte range check.
    WsFrameReader reader{/*expectMasked=*/true};
    reader.feed(encodeWsFrame(WsOpcode::kText, "\xF4\x90\x80\x80", /*mask=*/true));
    REQUIRE_THROWS_AS(reader.tryExtractFrame(), std::runtime_error);
}

TEST_CASE("WsFrameReader rejects an overlong 2-byte encoding", "[net][frame]") {
    // 0xC0 0x80 encodes U+0000, which fits in a single byte -- overlong.
    WsFrameReader reader{/*expectMasked=*/true};
    reader.feed(encodeWsFrame(WsOpcode::kText, "\xC0\x80", /*mask=*/true));
    REQUIRE_THROWS_AS(reader.tryExtractFrame(), std::runtime_error);
}

TEST_CASE("WsFrameReader rejects a 3-byte encoding of a UTF-16 surrogate half", "[net][frame]") {
    // 0xED 0xA0 0x80 decodes to U+D800, a surrogate half -- well-formed by the
    // byte-pattern rules alone, but never a legal Unicode scalar value.
    WsFrameReader reader{/*expectMasked=*/true};
    reader.feed(encodeWsFrame(WsOpcode::kText, "\xED\xA0\x80", /*mask=*/true));
    REQUIRE_THROWS_AS(reader.tryExtractFrame(), std::runtime_error);
}

TEST_CASE("WsFrameReader rejects an overlong 3-byte encoding", "[net][frame]") {
    // 0xE0 0x80 0x80 encodes U+0000, which fits in a single byte -- overlong.
    WsFrameReader reader{/*expectMasked=*/true};
    reader.feed(encodeWsFrame(WsOpcode::kText, "\xE0\x80\x80", /*mask=*/true));
    REQUIRE_THROWS_AS(reader.tryExtractFrame(), std::runtime_error);
}

TEST_CASE("WsFrameReader rejects invalid UTF-8 in the first fragment of a text message", "[net][frame]") {
    WsFrameReader reader{/*expectMasked=*/true};
    reader.feed(fragmentFrame(WsOpcode::kText, "\xFF", /*fin=*/false));
    REQUIRE_THROWS_AS(reader.tryExtractFrame(), std::runtime_error);
}

TEST_CASE("WsFrameReader rejects a fragmented text message ending mid multi-byte sequence", "[net][frame]") {
    WsFrameReader reader{/*expectMasked=*/true};
    reader.feed(fragmentFrame(WsOpcode::kText, "\xE2", /*fin=*/false));
    REQUIRE_FALSE(reader.tryExtractFrame().has_value());
    reader.feed(fragmentFrame(WsOpcode::kContinuation, "\x82", /*fin=*/true));  // still incomplete at FIN
    REQUIRE_THROWS_AS(reader.tryExtractFrame(), std::runtime_error);
}

TEST_CASE("WsFrameReader rejects a non-minimal 16-bit length encoding", "[net][frame]") {
    // FIN=1/text, unmasked, len-marker=126 declaring a 2-byte payload -- legal
    // encoders never emit this (2 fits the 7-bit field directly).
    std::string header;
    header.push_back(static_cast<char>(0x81));
    header.push_back(static_cast<char>(126));
    header.push_back(static_cast<char>(0x00));
    header.push_back(static_cast<char>(0x02));
    header += "hi";
    WsFrameReader reader{/*expectMasked=*/false};
    reader.feed(header);
    REQUIRE_THROWS_AS(reader.tryExtractFrame(), std::runtime_error);
}

TEST_CASE("WsFrameReader rejects a text frame with invalid UTF-8", "[net][frame]") {
    WsFrameReader reader{/*expectMasked=*/true};
    reader.feed(encodeWsFrame(WsOpcode::kText, "\xFF", /*mask=*/true));
    REQUIRE_THROWS_AS(reader.tryExtractFrame(), std::runtime_error);
}

TEST_CASE("WsFrameReader accepts a multi-byte UTF-8 sequence split across fragments", "[net][frame]") {
    std::string const euroSign = "\xE2\x82\xAC";  // U+20AC, 3 bytes
    WsFrameReader reader{/*expectMasked=*/true};
    reader.feed(fragmentFrame(WsOpcode::kText, euroSign.substr(0, 1), /*fin=*/false));
    reader.feed(fragmentFrame(WsOpcode::kContinuation, euroSign.substr(1), /*fin=*/true));
    auto const frame = requireFrame(reader);
    CHECK(frame.payload == euroSign);
}

TEST_CASE("WsFrameReader rejects a UTF-8 sequence broken across fragments", "[net][frame]") {
    WsFrameReader reader{/*expectMasked=*/true};
    reader.feed(fragmentFrame(WsOpcode::kText, "\xE2", /*fin=*/false));
    REQUIRE_FALSE(reader.tryExtractFrame().has_value());
    reader.feed(fragmentFrame(WsOpcode::kContinuation, "\x28\xAC", /*fin=*/true));  // 0x28 is not a continuation byte
    REQUIRE_THROWS_AS(reader.tryExtractFrame(), std::runtime_error);
}

TEST_CASE("WsFrameReader rejects a text message ending mid multi-byte sequence", "[net][frame]") {
    WsFrameReader reader{/*expectMasked=*/true};
    reader.feed(encodeWsFrame(WsOpcode::kText, "\xE2\x82", /*mask=*/true));  // FIN=1, incomplete 3-byte sequence
    REQUIRE_THROWS_AS(reader.tryExtractFrame(), std::runtime_error);
}

TEST_CASE("encodeWsFrame draws a fresh mask key every call", "[net][frame]") {
    std::string const wireA = encodeWsFrame(WsOpcode::kText, "same", /*mask=*/true);
    std::string const wireB = encodeWsFrame(WsOpcode::kText, "same", /*mask=*/true);
    CHECK(wireA.substr(2, 4) != wireB.substr(2, 4));  // bytes [2,6) are the mask key
}

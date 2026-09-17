// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <random>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>

#include "../../core/wire.hpp"

namespace morph::net::detail {

/// @brief RFC 6455 §5.2 frame opcodes this reference implementation understands.
enum class WsOpcode : std::uint8_t {
    kContinuation = 0x0,
    kText = 0x1,
    kBinary = 0x2,
    kClose = 0x8,
    kPing = 0x9,
    kPong = 0xA,
};

/// @brief One decoded (unfragmented) WebSocket frame.
struct WsFrame {
    /// @brief The frame's opcode.
    WsOpcode opcode{WsOpcode::kText};
    /// @brief The frame's (already-unmasked, if it was masked) payload bytes.
    std::string payload;
};

namespace ws_frame_impl {
// RFC 6455 §5.3 requires the mask key be unpredictable. A `std::mt19937`
// seeded once per thread is fully reconstructible from 624 observed 32-bit
// outputs, and a server that terminates many connections on one thread hands
// an attacker exactly that many keys. `std::random_device` is used instead:
// it has no reproducible internal state to recover, so a peer learns nothing
// about the next key from any number of observed ones.
//
// It is held `thread_local` rather than constructed per frame: construction
// is what acquires the entropy source (on libstdc++ without RDRAND/RDSEED,
// an `open()` of `/dev/urandom`), so a fresh object per frame would add an
// open/read/close round trip to *every* outbound message on the hot send
// path. A `thread_local` costs nothing in unpredictability -- `operator()`
// draws fresh entropy on each call regardless -- and one 32-bit draw already
// carries all four mask bytes.
inline std::array<std::uint8_t, 4> randomMaskKey() {
    static thread_local std::random_device entropy;
    auto const bits = static_cast<std::uint32_t>(entropy());
    return std::array<std::uint8_t, 4>{static_cast<std::uint8_t>(bits >> 24U), static_cast<std::uint8_t>(bits >> 16U),
                                       static_cast<std::uint8_t>(bits >> 8U), static_cast<std::uint8_t>(bits)};
}

/// @brief RFC 6455 §7.4.1 status codes a Close frame is allowed to carry.
/// 1004, 1005, 1006 and 1015 are explicitly reserved and MUST NOT appear on
/// the wire. 1000-1003 and 1007-1014 are assigned in the IANA WebSocket Close
/// Code Number Registry (1012 Service Restart, 1013 Try Again Later and 1014
/// Bad Gateway were registered after RFC 6455 shipped, and a proxy in front of
/// a real server does send them -- rejecting them would drop a legitimate
/// peer). 1015-2999 is unassigned registry space no endpoint may invent a code
/// in; 3000-4999 is reserved for libraries/frameworks and private use.
inline bool isValidCloseCode(std::uint16_t code) {
    if (code >= 1000 && code <= 1003) {
        return true;
    }
    if (code >= 1007 && code <= 1014) {
        return true;
    }
    return code >= 3000 && code <= 4999;
}

/// @brief Incremental UTF-8 validator for RFC 6455 §5.6's requirement that a
/// text message's payload be valid UTF-8 -- checked here because a multi-byte
/// sequence can straddle a fragment boundary, so validating each fragment in
/// isolation would reject legal input.
class Utf8Validator {
public:
    /// @return `false` as soon as a byte cannot extend valid UTF-8; once it
    ///         returns `false` the validator must not be fed further.
    bool feed(std::string_view data) {
        return std::ranges::all_of(data, [this](unsigned char currentByte) {
            return _remaining == 0 ? startSequence(currentByte) : continueSequence(currentByte);
        });
    }

    /// @return `true` if no multi-byte sequence was left dangling -- call at
    ///         the end of a (possibly reassembled) text message.
    [[nodiscard]] bool complete() const { return _remaining == 0; }

private:
    /// @brief Consumes a byte that starts a new (possibly single-byte) sequence.
    bool startSequence(unsigned char currentByte) {
        if (currentByte < 0x80) {
            return true;
        }
        if ((currentByte & 0xE0U) == 0xC0U) {
            if (currentByte < 0xC2U) {
                return false;  // overlong 2-byte lead (0xC0/0xC1)
            }
            _remaining = 1;
            _codepoint = currentByte & 0x1FU;
            _minCodepoint = 0x80;
            return true;
        }
        if ((currentByte & 0xF0U) == 0xE0U) {
            _remaining = 2;
            _codepoint = currentByte & 0x0FU;
            _minCodepoint = 0x800;
            return true;
        }
        if ((currentByte & 0xF8U) == 0xF0U) {
            if (currentByte > 0xF4U) {
                return false;  // beyond the U+10FFFF range
            }
            _remaining = 3;
            _codepoint = currentByte & 0x07U;
            _minCodepoint = 0x10000;
            return true;
        }
        return false;  // stray continuation byte or 0xF8-0xFF
    }

    /// @brief Consumes a byte expected to continue a sequence `startSequence` began.
    bool continueSequence(unsigned char currentByte) {
        if ((currentByte & 0xC0U) != 0x80U) {
            return false;  // expected a continuation byte
        }
        _codepoint = (_codepoint << 6) | (currentByte & 0x3FU);
        if (--_remaining != 0) {
            return true;
        }
        return _codepoint >= _minCodepoint && _codepoint <= 0x10FFFFU &&
               (_codepoint < 0xD800U || _codepoint > 0xDFFFU);  // overlong, out of range, or a surrogate half
    }

    unsigned _remaining{0};
    std::uint32_t _codepoint{0};
    std::uint32_t _minCodepoint{0};
};
}  // namespace ws_frame_impl

/// @brief Encodes one complete (unfragmented, `FIN=1`) WebSocket frame.
/// @param opcode  Frame opcode.
/// @param payload Payload bytes.
/// @param mask    `true` to mask the frame (required for client-to-server
///                frames per RFC 6455 §5.1); `false` for server-to-client frames.
/// @return The wire bytes of the frame, ready to send on the socket.
inline std::string encodeWsFrame(WsOpcode opcode, std::string_view payload, bool mask) {
    std::string out;
    out.push_back(static_cast<char>(0x80u | static_cast<std::uint8_t>(opcode)));  // FIN=1, opcode
    std::uint64_t const len = payload.size();
    std::uint8_t const maskBit = mask ? 0x80u : 0x00u;
    if (len <= 125) {
        out.push_back(static_cast<char>(maskBit | static_cast<std::uint8_t>(len)));
    } else if (len <= 0xFFFFu) {
        out.push_back(static_cast<char>(maskBit | 126u));
        out.push_back(static_cast<char>((len >> 8) & 0xFFu));
        out.push_back(static_cast<char>(len & 0xFFu));
    } else {
        out.push_back(static_cast<char>(maskBit | 127u));
        for (int shift = 56; shift >= 0; shift -= 8) {
            out.push_back(static_cast<char>((len >> shift) & 0xFFu));
        }
    }
    if (mask) {
        auto key = ws_frame_impl::randomMaskKey();
        for (auto b : key) {
            out.push_back(static_cast<char>(b));
        }
        for (std::size_t i = 0; i < payload.size(); ++i) {
            out.push_back(static_cast<char>(static_cast<std::uint8_t>(payload[i]) ^ key[i % key.size()]));
        }
    } else {
        out.append(payload);
    }
    return out;
}

/// @brief Streaming WebSocket message decoder over a byte buffer fed incrementally.
///
/// `encodeWsFrame` never fragments what *this* implementation sends — the
/// 64-bit extended-length field covers up to `wire::kMaxEnvelopeBytes`, so one
/// `wire::Envelope` always goes out as a single frame. Incoming traffic is a
/// different matter: a peer fragments according to *its* outgoing frame size,
/// and Qt's `QWebSocket` defaults that to 512 KiB. `tryExtractFrame` therefore
/// reassembles continuation frames and hands back only completed messages.
class WsFrameReader {
public:
    /// @param expectMasked RFC 6455 §5.1: a server MUST reject an unmasked
    ///        frame from a client, and a client MUST reject a masked frame
    ///        from a server. Pass `true` for a reader consuming client-to-
    ///        server traffic (i.e. inside a server), `false` for one
    ///        consuming server-to-client traffic (i.e. inside a client).
    explicit WsFrameReader(bool expectMasked) : _expectMasked(expectMasked) {}

    /// @brief Appends newly received bytes to the internal buffer.
    /// @param data Bytes read from the socket.
    void feed(std::string_view data) { _buf.append(data); }

    /// @brief Attempts to extract one complete *message* from the buffered bytes.
    ///
    /// Reassembles fragmented messages (RFC 6455 §5.4): a data message may
    /// arrive as an unfinished text/binary frame followed by continuation
    /// frames, with the last carrying FIN. Fragments are accumulated and only
    /// the completed message is returned, carrying the opcode of its first
    /// frame. This is not an exotic case — a peer fragments whenever a message
    /// exceeds its outgoing frame size, and Qt's `QWebSocket` defaults that to
    /// 512 KiB, so rejecting fragments broke interop with the transport this
    /// project ships for every payload past that size.
    ///
    /// Control frames (close/ping/pong) may be interleaved between the
    /// fragments of a data message and are returned as they arrive, without
    /// disturbing the reassembly in progress. Per the RFC they may not
    /// themselves be fragmented.
    ///
    /// @return The completed message if enough bytes are buffered, `std::nullopt`
    ///         otherwise.
    /// @throws std::runtime_error on a protocol violation (a continuation with
    ///         no message in progress, a new data frame interrupting one, a
    ///         fragmented control frame) or a payload — single frame or
    ///         reassembled total — larger than `wire::kMaxEnvelopeBytes`.
    std::optional<WsFrame> tryExtractFrame() {
        for (;;) {
            auto frame = tryExtractRawFrame();
            if (!frame) {
                return std::nullopt;
            }
            if (isControlOpcode(frame->opcode)) {
                return handleControlFrame(*frame);
            }
            if (frame->opcode == WsOpcode::kContinuation) {
                if (auto completed = handleContinuationFrame(*frame)) {
                    return completed;
                }
                continue;  // more fragments to come; keep draining the buffer
            }
            if (auto completed = handleDataFrame(*frame)) {
                return completed;
            }
            // else: `frame` started a fragmented message, now buffered internally.
        }
    }

private:
    /// @brief One frame exactly as it appeared on the wire, before any
    ///        fragmentation handling.
    struct RawFrame {
        bool fin;
        WsOpcode opcode;
        std::string payload;
    };

    // No `!frame.fin` check here: tryExtractRawFrame()'s validateControlFraming()
    // already rejects a fragmented control frame before a RawFrame is ever
    // returned, so every control frame reaching this point already has FIN=1.
    static WsFrame handleControlFrame(RawFrame& frame) {
        return WsFrame{.opcode = frame.opcode, .payload = std::move(frame.payload)};
    }

    /// @return The completed message once its last continuation (FIN=1)
    ///         arrives; `std::nullopt` while more fragments are expected.
    std::optional<WsFrame> handleContinuationFrame(RawFrame& frame) {
        if (!_assembling) {
            throw std::runtime_error("WsFrameReader: continuation frame with no message in progress");
        }
        if (_assemblyIsText && !_textUtf8.feed(frame.payload)) {
            resetAssembly();
            throw std::runtime_error("WsFrameReader: text message contains invalid UTF-8");
        }
        appendFragment(frame.payload);
        if (!frame.fin) {
            return std::nullopt;
        }
        if (_assemblyIsText && !_textUtf8.complete()) {
            resetAssembly();
            throw std::runtime_error("WsFrameReader: text message ends mid-UTF-8-sequence");
        }
        WsFrame completed{.opcode = _assemblyOpcode, .payload = std::move(_assembly)};
        resetAssembly();
        return completed;
    }

    /// @brief Handles a data (text/binary) frame, which starts a new message.
    /// @return The message immediately if `frame` was unfragmented (FIN=1);
    ///         `std::nullopt` if it started a fragmented message instead.
    std::optional<WsFrame> handleDataFrame(RawFrame& frame) {
        if (_assembling) {
            throw std::runtime_error("WsFrameReader: new data frame while a fragmented message is in progress");
        }
        if (frame.opcode == WsOpcode::kText && frame.fin) {
            ws_frame_impl::Utf8Validator validator;
            if (!validator.feed(frame.payload) || !validator.complete()) {
                throw std::runtime_error("WsFrameReader: text message contains invalid UTF-8");
            }
        }
        if (frame.fin) {
            return WsFrame{.opcode = frame.opcode,
                           .payload = std::move(frame.payload)};  // the common, unfragmented case
        }
        _assembling = true;
        _assemblyOpcode = frame.opcode;
        _assemblyIsText = (frame.opcode == WsOpcode::kText);
        _textUtf8 = ws_frame_impl::Utf8Validator{};
        if (_assemblyIsText && !_textUtf8.feed(frame.payload)) {
            resetAssembly();
            throw std::runtime_error("WsFrameReader: text message contains invalid UTF-8");
        }
        _assembly.clear();
        appendFragment(frame.payload);
        return std::nullopt;
    }

    void resetAssembly() {
        _assembling = false;
        _assemblyIsText = false;
        _textUtf8 = ws_frame_impl::Utf8Validator{};
        _assembly.clear();
        _assembly.shrink_to_fit();
    }

    void appendFragment(const std::string& payload) {
        // Bound the reassembled total, not just each frame: without this, a
        // peer could stream unlimited 1-byte continuations and grow this buffer
        // without ever tripping the per-frame cap.
        if (_assembly.size() + payload.size() > ::morph::wire::kMaxEnvelopeBytes) {
            resetAssembly();
            throw std::runtime_error("WsFrameReader: reassembled message exceeds kMaxEnvelopeBytes");
        }
        _assembly += payload;
    }

    /// @brief Validates RSV/opcode and returns the opcode, or throws.
    static WsOpcode validateFirstByte(std::uint8_t byte0) {
        if ((byte0 & 0x70U) != 0) {
            throw std::runtime_error("WsFrameReader: RSV bit set with no extension negotiated");
        }
        auto const opcode = static_cast<WsOpcode>(static_cast<std::uint8_t>(byte0 & 0x0FU));
        switch (opcode) {
            case WsOpcode::kContinuation:
            case WsOpcode::kText:
            case WsOpcode::kBinary:
            case WsOpcode::kClose:
            case WsOpcode::kPing:
            case WsOpcode::kPong:
                return opcode;
            default:
                throw std::runtime_error("WsFrameReader: reserved opcode");
        }
    }

    void validateMaskDirection(bool masked) const {
        if (masked != _expectMasked) {
            throw std::runtime_error(_expectMasked ? "WsFrameReader: unmasked frame from a client"
                                                   : "WsFrameReader: masked frame from a server");
        }
    }

    static bool isControlOpcode(WsOpcode opcode) { return (static_cast<std::uint8_t>(opcode) & 0x08U) != 0; }

    static void validateControlFraming(bool isControl, bool fin, std::uint8_t lenMarker) {
        if (!isControl) {
            return;
        }
        if (!fin) {
            throw std::runtime_error("WsFrameReader: control frames must not be fragmented");
        }
        if (lenMarker > 125) {
            throw std::runtime_error("WsFrameReader: control frame payload exceeds 125 bytes");
        }
    }

    /// @brief The result of decoding a (possibly extended) length field.
    struct LengthDecode {
        std::uint64_t payloadLen;
        std::size_t headerLen;
    };

    /// @return `std::nullopt` if the extended-length bytes are not fully
    ///         buffered yet; throws on a non-minimal encoding.
    [[nodiscard]] std::optional<LengthDecode> decodeLength(std::uint8_t lenMarker) const {
        std::uint64_t payloadLen = lenMarker;
        std::size_t headerLen = 2;
        if (payloadLen == 126) {
            if (_buf.size() < 4) {
                return std::nullopt;
            }
            payloadLen = (static_cast<std::uint64_t>(static_cast<std::uint8_t>(_buf[2])) << 8) |
                         static_cast<std::uint64_t>(static_cast<std::uint8_t>(_buf[3]));
            if (payloadLen <= 125) {
                throw std::runtime_error("WsFrameReader: non-minimal 16-bit length encoding");
            }
            headerLen = 4;
        } else if (payloadLen == 127) {
            if (_buf.size() < 10) {
                return std::nullopt;
            }
            payloadLen = 0;
            for (std::size_t i = 0; i < 8; ++i) {
                payloadLen = (payloadLen << 8) | static_cast<std::uint8_t>(_buf[2 + i]);
            }
            if (payloadLen <= 0xFFFFU) {
                throw std::runtime_error("WsFrameReader: non-minimal 64-bit length encoding");
            }
            headerLen = 10;
        }
        return LengthDecode{.payloadLen = payloadLen, .headerLen = headerLen};
    }

    /// @brief Copies out and (if masked) unmasks the payload bytes.
    [[nodiscard]] std::string extractPayload(std::size_t headerLen, std::size_t maskLen, std::size_t payloadLen,
                                             bool masked) const {
        std::string payload = _buf.substr(headerLen + maskLen, payloadLen);
        if (masked) {
            std::array<std::uint8_t, 4> key{};
            for (std::size_t i = 0; i < 4; ++i) {
                key[i] = static_cast<std::uint8_t>(_buf[headerLen + i]);
            }
            for (std::size_t i = 0; i < payload.size(); ++i) {
                payload[i] = static_cast<char>(static_cast<std::uint8_t>(payload[i]) ^ key[i % key.size()]);
            }
        }
        return payload;
    }

    static void validateClosePayload(const std::string& payload) {
        if (payload.empty()) {
            return;  // a Close with no body is legal and carries no status code
        }
        bool const validClosePayload =
            payload.size() >= 2 &&
            ws_frame_impl::isValidCloseCode(static_cast<std::uint16_t>(
                (static_cast<std::uint8_t>(payload.at(0)) << 8) | static_cast<std::uint8_t>(payload.at(1))));
        if (!validClosePayload) {
            throw std::runtime_error("WsFrameReader: close frame carries an invalid status code");
        }
        // RFC 6455 §5.5.1: whatever follows the two status-code bytes is a
        // human-readable reason that MUST be valid UTF-8, exactly as a Text
        // message's payload must be. A Close body is never fragmented (control
        // frames always carry FIN=1), so one self-contained pass suffices.
        ws_frame_impl::Utf8Validator reasonUtf8;
        if (!reasonUtf8.feed(std::string_view{payload}.substr(2)) || !reasonUtf8.complete()) {
            throw std::runtime_error("WsFrameReader: close frame reason is not valid UTF-8");
        }
    }

    std::optional<RawFrame> tryExtractRawFrame() {
        if (_buf.size() < 2) {
            return std::nullopt;
        }
        auto const byte0 = static_cast<std::uint8_t>(_buf.at(0));
        auto const byte1 = static_cast<std::uint8_t>(_buf.at(1));
        auto const opcode = validateFirstByte(byte0);
        bool const fin = (byte0 & 0x80U) != 0;
        bool const masked = (byte1 & 0x80U) != 0;
        validateMaskDirection(masked);
        std::uint8_t const lenMarker = byte1 & 0x7FU;
        validateControlFraming(isControlOpcode(opcode), fin, lenMarker);

        auto const decoded = decodeLength(lenMarker);
        if (!decoded) {
            return std::nullopt;
        }
        if (decoded->payloadLen > ::morph::wire::kMaxEnvelopeBytes) {
            throw std::runtime_error("WsFrameReader: frame payload exceeds kMaxEnvelopeBytes");
        }
        std::size_t const maskLen = masked ? 4 : 0;
        std::size_t const totalLen = decoded->headerLen + maskLen + static_cast<std::size_t>(decoded->payloadLen);
        if (_buf.size() < totalLen) {
            return std::nullopt;
        }
        std::string payload =
            extractPayload(decoded->headerLen, maskLen, static_cast<std::size_t>(decoded->payloadLen), masked);
        if (opcode == WsOpcode::kClose) {
            validateClosePayload(payload);
        }
        _buf.erase(0, totalLen);
        return RawFrame{.fin = fin, .opcode = opcode, .payload = std::move(payload)};
    }

    bool _expectMasked;
    std::string _buf;
    // Reassembly state for a fragmented data message: `_assembly` accumulates
    // the payload and `_assemblyOpcode` remembers the opcode of the first
    // frame, since continuations carry opcode 0. `_assemblyIsText` and
    // `_textUtf8` track incremental UTF-8 validity across fragments, since a
    // multi-byte sequence can straddle a fragment boundary.
    bool _assembling{false};
    WsOpcode _assemblyOpcode{WsOpcode::kText};
    bool _assemblyIsText{false};
    ws_frame_impl::Utf8Validator _textUtf8;
    std::string _assembly;
};

}  // namespace morph::net::detail

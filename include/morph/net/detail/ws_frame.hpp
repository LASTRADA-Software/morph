// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <random>
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
inline std::array<std::uint8_t, 4> randomMaskKey() {
    static thread_local std::mt19937 gen{std::random_device{}()};
    std::uniform_int_distribution<int> dist{0, 255};
    std::array<std::uint8_t, 4> key{};
    for (auto& b : key) {
        b = static_cast<std::uint8_t>(dist(gen));
    }
    return key;
}
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

/// @brief Streaming WebSocket frame decoder over a byte buffer fed incrementally.
///
/// Only single-frame (`FIN=1`) messages are supported — sufficient for
/// `wire::Envelope` JSON, which is always sent as one frame; the 64-bit
/// extended-length field already covers up to `wire::kMaxEnvelopeBytes`, so no
/// message ever needs fragmentation. A fragmented frame (`FIN=0` or a
/// `CONTINUATION` opcode) throws rather than silently mis-parsing.
class WsFrameReader {
public:
    /// @brief Appends newly received bytes to the internal buffer.
    /// @param data Bytes read from the socket.
    void feed(std::string_view data) { _buf.append(data); }

    /// @brief Attempts to extract one complete frame from the buffered bytes.
    /// @return The frame if enough bytes are buffered, `std::nullopt` otherwise.
    /// @throws std::runtime_error on a fragmented frame or a payload declared
    ///         larger than `wire::kMaxEnvelopeBytes`.
    std::optional<WsFrame> tryExtractFrame() {
        if (_buf.size() < 2) {
            return std::nullopt;
        }
        auto const byte0 = static_cast<std::uint8_t>(_buf[0]);
        auto const byte1 = static_cast<std::uint8_t>(_buf[1]);
        bool const fin = (byte0 & 0x80u) != 0;
        auto const opcode = static_cast<WsOpcode>(static_cast<std::uint8_t>(byte0 & 0x0Fu));
        if (!fin || opcode == WsOpcode::kContinuation) {
            throw std::runtime_error("WsFrameReader: fragmented frames are not supported");
        }
        bool const masked = (byte1 & 0x80u) != 0;
        std::uint64_t payloadLen = byte1 & 0x7Fu;
        std::size_t headerLen = 2;
        if (payloadLen == 126) {
            if (_buf.size() < 4) {
                return std::nullopt;
            }
            payloadLen = (static_cast<std::uint64_t>(static_cast<std::uint8_t>(_buf[2])) << 8) |
                         static_cast<std::uint64_t>(static_cast<std::uint8_t>(_buf[3]));
            headerLen = 4;
        } else if (payloadLen == 127) {
            if (_buf.size() < 10) {
                return std::nullopt;
            }
            payloadLen = 0;
            for (std::size_t i = 0; i < 8; ++i) {
                payloadLen = (payloadLen << 8) | static_cast<std::uint8_t>(_buf[2 + i]);
            }
            headerLen = 10;
        }
        if (payloadLen > ::morph::wire::kMaxEnvelopeBytes) {
            throw std::runtime_error("WsFrameReader: frame payload exceeds kMaxEnvelopeBytes");
        }
        std::size_t const maskLen = masked ? 4 : 0;
        std::size_t const totalLen = headerLen + maskLen + static_cast<std::size_t>(payloadLen);
        if (_buf.size() < totalLen) {
            return std::nullopt;
        }
        std::string payload = _buf.substr(headerLen + maskLen, static_cast<std::size_t>(payloadLen));
        if (masked) {
            std::array<std::uint8_t, 4> key{};
            for (std::size_t i = 0; i < 4; ++i) {
                key[i] = static_cast<std::uint8_t>(_buf[headerLen + i]);
            }
            for (std::size_t i = 0; i < payload.size(); ++i) {
                payload[i] = static_cast<char>(static_cast<std::uint8_t>(payload[i]) ^ key[i % key.size()]);
            }
        }
        _buf.erase(0, totalLen);
        return WsFrame{opcode, std::move(payload)};
    }

private:
    std::string _buf;
};

}  // namespace morph::net::detail

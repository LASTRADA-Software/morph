// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <QtGlobal>
#include <chrono>
#include <cstddef>
#include <deque>
#include <optional>

/// @file
/// @brief Which frames handed to a socket it has written, told from the
///        socket's own byte counts.

namespace morph::qt::detail {

/// @brief Frames handed to one socket and not yet written, oldest first, each
///        with the position in the socket's running count of written bytes at
///        which its last byte is out.
///
/// A frame's position is taken when it is handed over: the bytes written so
/// far, plus every byte the socket still holds, the frame's own included. So
/// it is measured, not computed from a frame layout, and covers whatever the
/// socket queued for itself ahead of it. Over TLS the bytes held must include
/// the ciphertext not yet sent as well as the plaintext not yet encrypted;
/// plaintext is held unencrypted only briefly, so a frame can be released
/// ahead of its last byte by at most the record overhead on what was then
/// unencrypted, and the error does not accumulate: each position is taken
/// afresh.
///
/// Not synchronised: owned by the socket's thread.
class OutboundLedger {
public:
    /// @brief Records a frame just handed to the socket.
    /// @param pending  Bytes the socket holds after taking the frame,
    ///                 the frame's own included.
    /// @param counted  What the frame added to the connection's backlog.
    /// @param queuedAt When the frame was handed over.
    void push(qint64 pending, std::size_t counted, std::chrono::steady_clock::time_point queuedAt) {
        _frames.push_back(Frame{.end = _written + pending, .counted = counted, .queuedAt = queuedAt});
    }

    /// @brief Advances the written count and releases every frame it completes.
    /// @param bytes   Bytes the socket just reported written.
    /// @param pending Bytes the socket still holds after writing them. `0`
    ///                releases every frame, whatever its position.
    /// @return The sum of the released frames' `counted`.
    [[nodiscard]] std::size_t written(qint64 bytes, qint64 pending) {
        _written += bytes;
        std::size_t released = 0;
        while (!_frames.empty() && (pending == 0 || _frames.front().end <= _written)) {
            released += _frames.front().counted;
            _frames.pop_front();
        }
        return released;
    }

    /// @brief Forgets every frame, as when the connection goes away.
    /// @return The sum of the forgotten frames' `counted`.
    [[nodiscard]] std::size_t clear() {
        std::size_t dropped = 0;
        for (auto const& frame : _frames) {
            dropped += frame.counted;
        }
        _frames.clear();
        return dropped;
    }

    /// @brief When the oldest unwritten frame was handed over.
    /// @return Its time, or `std::nullopt` when every frame is written.
    [[nodiscard]] std::optional<std::chrono::steady_clock::time_point> oldest() const {
        if (_frames.empty()) {
            return std::nullopt;
        }
        return _frames.front().queuedAt;
    }

private:
    /// @brief One frame handed to the socket and not yet written.
    struct Frame {
        /// @brief Written count at which the frame's last byte is out.
        qint64 end = 0;
        /// @brief What the frame added to the backlog: its envelope's size.
        std::size_t counted = 0;
        /// @brief When the frame was handed over.
        std::chrono::steady_clock::time_point queuedAt;
    };

    std::deque<Frame> _frames;
    qint64 _written = 0;
};

}  // namespace morph::qt::detail

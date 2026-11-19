// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <cstddef>
#include <string_view>

namespace morph::ui {

/// @brief Where a text cursor asked to sit at byte @p offset of @p text lands: at the text's end for an offset past
///        it, and at the start of the code point for an offset inside a UTF-8 sequence.
///
/// Every backend places a cursor through this, so they agree on an offset that is not at a code point's start.
/// @param text   The text the cursor is in, UTF-8.
/// @param offset The requested byte offset.
/// @return A byte offset at most @p offset, at most `text.size()`, and at the start of a code point.
[[nodiscard]] constexpr std::size_t snapToCodePointStart(std::string_view text, std::size_t offset) noexcept {
    std::size_t at = std::min(offset, text.size());
    // A continuation byte is 0b10xxxxxx.
    while (at > 0 && at < text.size() && (static_cast<unsigned char>(text[at]) & 0b1100'0000U) == 0b1000'0000U) {
        --at;
    }
    return at;
}

}  // namespace morph::ui

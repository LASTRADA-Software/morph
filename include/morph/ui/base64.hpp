// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <cstddef>
#include <morph/export.hpp>
#include <optional>
#include <string>
#include <string_view>

/// @file
/// @brief Base64 (RFC 4648), the encoding a document's inline file and a dictated clip travel in.

namespace morph::ui::detail {

/// @brief Bytes as base64 (RFC 4648, with padding): how a document carries an inline file, and how a dictated clip
///        travels.
/// @param bytes The bytes.
/// @return The text.
[[nodiscard]] MORPH_API std::string base64Encode(std::string_view bytes);

/// @brief Base64 text as bytes (RFC 4648, with or without padding).
/// @param text The text.
/// @return The bytes, or `std::nullopt` for text that is not base64.
[[nodiscard]] MORPH_API std::optional<std::string> base64Decode(std::string_view text);

/// @brief How many bytes base64 text decodes to, read from its length alone.
/// @param text The text.
/// @return The byte count.
[[nodiscard]] MORPH_API std::size_t base64Size(std::string_view text);

}  // namespace morph::ui::detail

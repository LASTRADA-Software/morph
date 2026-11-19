// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <morph/export.hpp>
#include <optional>
#include <string>
#include <string_view>

/// @file
/// @brief A document's JSON text as the source view shows it: pretty-printed compactly, and the place a JSON Pointer
/// names in
///        it, so a refusal moves the cursor to the part it is about.
///
/// Specified in `docs/spec/ui/custom_screens.md`, "The client".

namespace morph::ui {

/// @brief @p text pretty-printed for reading: indented by 2, with a container that fits within about 100 columns on
///        one line (`{"kind": "text", "text": "x"}`) and every other member or element on a line of its own.
///
/// Members keep their order and scalars their written form, so the text means what @p text meant and is several times
/// smaller than one value per line.
/// @param text JSON text.
/// @return The pretty text; @p text unchanged when it is not JSON, so a hand-edited draft that does not parse is shown
///         as the person left it.
[[nodiscard]] MORPH_API std::string prettyJson(std::string_view text);

/// @brief @p text with the white space between tokens removed, so a document indented for reading is sent as the bytes
///        a bound was meant for.
///
/// Strings are untouched and members stay in their order; nothing about the document changes. A JSON Pointer into the
/// result is the one into @p text, but a byte offset is not.
/// @param text JSON text.
/// @return The compact text; @p text unchanged when it is not JSON, so the server refuses what the person wrote.
[[nodiscard]] MORPH_API std::string compactJson(std::string_view text);

/// @brief Where the part a JSON Pointer names starts in @p text.
///
/// The text is scanned, not parsed into a value, so the offset is into the text as it is, with its own spacing and
/// member order. For a member, the offset is that of its name's opening quote, where a person looks for it; for an
/// array element, that of the element's first byte. Of equal member names the last counts, as it does in the parsed
/// value. The empty pointer answers the first byte that is not white space, whether or not the text is JSON; a byte
/// order mark before the document is not white space, so a non-empty pointer finds nothing after one.
/// @param text    JSON text.
/// @param pointer An RFC 6901 pointer; `~0` and `~1` are unescaped.
/// @return The byte offset, or `std::nullopt` when the pointer names nothing in @p text, @p text is not JSON up
///         to that part, the pointer is not one, or it reaches deeper than `kMaxJsonDepth`.
[[nodiscard]] MORPH_API std::optional<std::size_t> pointerOffset(std::string_view text, std::string_view pointer);

/// @brief The line @p offset is on.
/// @param text   The text.
/// @param offset A byte offset into it; past its end counts as its end.
/// @return The line, counting from 1.
[[nodiscard]] MORPH_API std::size_t lineAt(std::string_view text, std::size_t offset);

}  // namespace morph::ui

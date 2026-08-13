// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace bookmarks::import {

/// @brief One parsed `<A HREF="...">title</A>` entry. `url` empty means
///        "malformed, skip" — the caller (`BookmarkModel::execute(const
///        ImportBookmarks&)`) counts these toward `skipped`, not `imported`.
struct ParsedEntry {
    std::string url;
    std::string title;
};

/// @brief Extracts every `<A HREF="...">...</A>` entry from one Netscape
///        Bookmark File chunk. Deliberately minimal: recognizes `HREF`
///        case-insensitively, decodes the five predefined XML entities in
///        both the `HREF` value and the title text (symmetric with
///        `escapeHtml`, which `ExportBookmarks` applies to both), and
///        tolerates (by skipping) an `<A>` with no `HREF` attribute or an
///        unterminated tag. A URL therefore survives an export/reimport
///        round trip unchanged, including URLs containing `&`, `<`, `>`,
///        `"`, or `'`. Anything this rung's own `ExportBookmarks` never
///        produces (nested tags inside the title) is out of scope by
///        design, not an oversight.
/// @param chunk Raw HTML/text to scan.
/// @return Every entry found, in document order.
[[nodiscard]] std::vector<ParsedEntry> parseNetscapeChunk(std::string_view chunk);

/// @brief Escapes `&`, `<`, `>`, `"`, and `'` for safe inclusion in
///        generated Netscape Bookmark File output.
/// @param text Raw text to escape.
/// @return The escaped text.
[[nodiscard]] std::string escapeHtml(std::string_view text);

}  // namespace bookmarks::import

// SPDX-License-Identifier: Apache-2.0
#include "bookmarks/import/netscape_bookmarks.hpp"

#include <cctype>
#include <cstddef>

namespace bookmarks::import {

namespace {

[[nodiscard]] std::string decodeEntities(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        if (text[i] == '&') {
            if (text.substr(i, 5) == "&amp;") {
                out += '&';
                i += 5;
                continue;
            }
            if (text.substr(i, 4) == "&lt;") {
                out += '<';
                i += 4;
                continue;
            }
            if (text.substr(i, 4) == "&gt;") {
                out += '>';
                i += 4;
                continue;
            }
            if (text.substr(i, 6) == "&quot;") {
                out += '"';
                i += 6;
                continue;
            }
            if (text.substr(i, 5) == "&#39;") {
                out += '\'';
                i += 5;
                continue;
            }
        }
        out += text[i];
        ++i;
    }
    return out;
}

/// @brief Case-insensitive substring search for @p needle in @p haystack,
///        starting at @p from.
[[nodiscard]] std::size_t findCaseInsensitive(std::string_view haystack, std::string_view needle, std::size_t from) {
    if (needle.empty() || needle.size() > haystack.size()) {
        return std::string_view::npos;
    }
    for (std::size_t i = from; i + needle.size() <= haystack.size(); ++i) {
        bool match = true;
        for (std::size_t j = 0; j < needle.size(); ++j) {
            if (std::tolower(static_cast<unsigned char>(haystack[i + j])) !=
                std::tolower(static_cast<unsigned char>(needle[j]))) {
                match = false;
                break;
            }
        }
        if (match) {
            return i;
        }
    }
    return std::string_view::npos;
}

}  // namespace

std::vector<ParsedEntry> parseNetscapeChunk(std::string_view chunk) {
    std::vector<ParsedEntry> entries;
    std::size_t pos = 0;
    while (true) {
        const auto tagStart = findCaseInsensitive(chunk, "<a", pos);
        if (tagStart == std::string_view::npos) {
            break;
        }
        const auto tagEnd = chunk.find('>', tagStart);
        if (tagEnd == std::string_view::npos) {
            break;  // unterminated tag -- nothing more to parse in this chunk
        }
        const auto closeStart = findCaseInsensitive(chunk, "</a>", tagEnd);
        if (closeStart == std::string_view::npos) {
            break;  // unterminated element
        }

        const std::string_view attrs = chunk.substr(tagStart, tagEnd - tagStart);
        ParsedEntry entry;
        const auto hrefPos = findCaseInsensitive(attrs, "href=", 0);
        if (hrefPos != std::string_view::npos) {
            auto valueStart = hrefPos + 5;
            if (valueStart < attrs.size() && attrs[valueStart] == '"') {
                const auto valueEnd = attrs.find('"', valueStart + 1);
                if (valueEnd != std::string_view::npos) {
                    entry.url = decodeEntities(attrs.substr(valueStart + 1, valueEnd - valueStart - 1));
                }
            }
        }
        entry.title = decodeEntities(chunk.substr(tagEnd + 1, closeStart - tagEnd - 1));
        entries.push_back(std::move(entry));

        pos = closeStart + 4;
    }
    return entries;
}

std::string escapeHtml(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char ch : text) {
        switch (ch) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default: out += ch;
        }
    }
    return out;
}

}  // namespace bookmarks::import

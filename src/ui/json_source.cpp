// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <glaze/glaze.hpp>
#include <morph/ui/json_source.hpp>
#include <morph/ui/value.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace morph::ui {

namespace {

constexpr bool isSpace(char byte) noexcept { return byte == ' ' || byte == '\n' || byte == '\r' || byte == '\t'; }

std::size_t skipSpace(std::string_view text, std::size_t at) {
    while (at < text.size() && isSpace(text[at])) {
        ++at;
    }
    return at;
}

char unescaped(char escape) noexcept {
    switch (escape) {
        case 'n':
            return '\n';
        case 't':
            return '\t';
        case 'r':
            return '\r';
        case 'b':
            return '\b';
        case 'f':
            return '\f';
        default:
            return escape;  // `"`, `\` and `/` stand for themselves
    }
}

// A string at its opening quote: the offset past its closing quote. Its content is appended to @p out when one is
// given, escapes decoded except `\u`, which is kept as written: the vocabulary's member names are ASCII.
std::optional<std::size_t> scanString(std::string_view text, std::size_t at, std::string* out) {
    if (at >= text.size() || text[at] != '"') {
        return std::nullopt;
    }
    for (++at; at < text.size();) {
        char const byte = text[at];
        if (byte == '"') {
            return at + 1;
        }
        if (byte != '\\') {
            if (out != nullptr) {
                out->push_back(byte);
            }
            ++at;
            continue;
        }
        if (at + 1 >= text.size()) {
            return std::nullopt;
        }
        std::size_t const length = text[at + 1] == 'u' ? 6 : 2;
        if (at + length > text.size()) {
            return std::nullopt;
        }
        if (out != nullptr) {
            if (length == 6) {
                out->append(text.substr(at, length));
            } else {
                out->push_back(unescaped(text[at + 1]));
            }
        }
        at += length;
    }
    return std::nullopt;
}

std::optional<std::size_t> skipValue(std::string_view text, std::size_t at, std::size_t depth);

// The container at @p at, skipped: the offset past its closing bracket.
// NOLINTNEXTLINE(misc-no-recursion): a container's values, bounded by kMaxJsonDepth in skipValue.
std::optional<std::size_t> skipContainer(std::string_view text, std::size_t at, std::size_t depth) {
    char const close = text[at] == '{' ? '}' : ']';
    at = skipSpace(text, at + 1);
    if (at < text.size() && text[at] == close) {
        return at + 1;
    }
    while (at < text.size()) {
        if (close == '}') {
            std::optional<std::size_t> const name = scanString(text, at, nullptr);
            if (!name) {
                return std::nullopt;
            }
            at = skipSpace(text, *name);
            if (at >= text.size() || text[at] != ':') {
                return std::nullopt;
            }
            at = skipSpace(text, at + 1);
        }
        std::optional<std::size_t> const value = skipValue(text, at, depth + 1);
        if (!value) {
            return std::nullopt;
        }
        at = skipSpace(text, *value);
        if (at < text.size() && text[at] == ',') {
            at = skipSpace(text, at + 1);
            continue;
        }
        if (at < text.size() && text[at] == close) {
            return at + 1;
        }
        return std::nullopt;
    }
    return std::nullopt;
}

// Any value at @p at, skipped: the offset past it.
// NOLINTNEXTLINE(misc-no-recursion): a container's values, bounded by kMaxJsonDepth.
std::optional<std::size_t> skipValue(std::string_view text, std::size_t at, std::size_t depth) {
    if (depth > kMaxJsonDepth || at >= text.size()) {
        return std::nullopt;
    }
    char const first = text[at];
    if (first == '"') {
        return scanString(text, at, nullptr);
    }
    if (first == '{' || first == '[') {
        return skipContainer(text, at, depth);
    }
    // A number or a literal runs to the next delimiter.
    std::size_t end = at;
    while (end < text.size() && !isSpace(text[end]) && text[end] != ',' && text[end] != '}' && text[end] != ']') {
        ++end;
    }
    return end == at ? std::nullopt : std::optional<std::size_t>{end};
}

// The pointer's reference tokens, unescaped; nothing for text that is not a pointer.
std::optional<std::vector<std::string>> tokensOf(std::string_view pointer) {
    std::vector<std::string> tokens;
    if (pointer.empty()) {
        return tokens;
    }
    if (pointer.front() != '/') {
        return std::nullopt;
    }
    std::string token;
    for (std::size_t at = 1; at <= pointer.size(); ++at) {
        if (at == pointer.size() || pointer[at] == '/') {
            tokens.push_back(std::move(token));
            token.clear();
            continue;
        }
        if (pointer[at] == '~') {
            if (at + 1 >= pointer.size() || (pointer[at + 1] != '0' && pointer[at + 1] != '1')) {
                return std::nullopt;
            }
            token.push_back(pointer[at + 1] == '0' ? '~' : '/');
            ++at;
            continue;
        }
        token.push_back(pointer[at]);
    }
    return tokens;
}

// An array index token: digits, with no leading zero but for `0` itself.
std::optional<std::size_t> indexOf(std::string const& token) {
    if (token.empty() || (token.size() > 1 && token.front() == '0')) {
        return std::nullopt;
    }
    std::size_t index = 0;
    auto const [end, error] = std::from_chars(token.data(), token.data() + token.size(), index);
    if (error != std::errc{} || end != token.data() + token.size()) {
        return std::nullopt;
    }
    return index;
}

}  // namespace

std::string prettyJson(std::string_view text) {
    if (!parseJson(text)) {
        return std::string{text};
    }
    std::string out;
    glz::prettify_json(text, out);
    return out;
}

std::optional<std::size_t> pointerOffset(std::string_view text, std::string_view pointer) {
    std::optional<std::vector<std::string>> const tokens = tokensOf(pointer);
    if (!tokens) {
        return std::nullopt;
    }
    std::size_t value = skipSpace(text, 0);  // where the value reached so far starts
    std::size_t shown = value;               // where a person sees it: its member's name, or the value itself
    for (std::size_t depth = 0; depth < tokens->size(); ++depth) {
        if (depth >= kMaxJsonDepth || value >= text.size() || (text[value] != '{' && text[value] != '[')) {
            return std::nullopt;
        }
        std::string const& token = tokens->at(depth);
        bool const object = text[value] == '{';
        std::optional<std::size_t> const wanted = object ? std::nullopt : indexOf(token);
        if (!object && !wanted) {
            return std::nullopt;
        }
        std::size_t at = skipSpace(text, value + 1);
        std::size_t place = 0;
        bool found = false;
        while (at < text.size() && text[at] != (object ? '}' : ']')) {
            std::size_t const entry = at;
            bool matches = false;
            if (object) {
                std::string name;
                std::optional<std::size_t> const after = scanString(text, at, &name);
                if (!after) {
                    return std::nullopt;
                }
                at = skipSpace(text, *after);
                if (at >= text.size() || text[at] != ':') {
                    return std::nullopt;
                }
                at = skipSpace(text, at + 1);
                matches = name == token;
            } else {
                matches = place == *wanted;
            }
            if (matches) {
                value = at;
                shown = entry;
                found = true;
                break;
            }
            std::optional<std::size_t> const skipped = skipValue(text, at, depth + 1);
            if (!skipped) {
                return std::nullopt;
            }
            at = skipSpace(text, *skipped);
            if (at < text.size() && text[at] == ',') {
                at = skipSpace(text, at + 1);
            }
            ++place;
        }
        if (!found) {
            return std::nullopt;
        }
    }
    return shown;
}

std::size_t lineAt(std::string_view text, std::size_t offset) {
    std::string_view const before = text.substr(0, std::min(offset, text.size()));
    return 1 + static_cast<std::size_t>(std::ranges::count(before, '\n'));
}

}  // namespace morph::ui

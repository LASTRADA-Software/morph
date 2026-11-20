// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <glaze/glaze.hpp>
#include <memory>
#include <morph/ui/json_source.hpp>
#include <morph/ui/value.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace morph::ui {

namespace {

constexpr bool isSpace(char byte) noexcept { return byte == ' ' || byte == '\n' || byte == '\r' || byte == '\t'; }

std::size_t skipSpace(std::string_view text, std::size_t pos) {
    while (pos < text.size() && isSpace(text.at(pos))) {
        ++pos;
    }
    return pos;
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

std::optional<unsigned> hex4(std::string_view text, std::size_t pos) {
    if (pos + 4 > text.size()) {
        return std::nullopt;
    }
    unsigned code = 0;
    std::string_view const digits = text.substr(pos, 4);
    auto const [end, error] =
        std::from_chars(std::to_address(digits.begin()), std::to_address(digits.end()), code, 16);
    if (error != std::errc{} || end != std::to_address(digits.end())) {
        return std::nullopt;
    }
    return code;
}

// The `\uXXXX` at @p pos skipped: how many bytes it takes.
std::optional<std::size_t> skipUnicodeEscape(std::string_view text, std::size_t pos) {
    return pos + 6 <= text.size() ? std::optional<std::size_t>{6} : std::nullopt;
}

void appendUtf8(std::string& out, unsigned code) {
    if (code < 0x80) {
        out.push_back(static_cast<char>(code));
    } else if (code < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (code >> 6)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else if (code < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (code >> 12)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (code >> 18)));
        out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    }
}

// The `\uXXXX` at @p pos (with its low surrogate when it opens a pair) appended to @p out as UTF-8: how many bytes of
// text it took.
std::optional<std::size_t> appendUnicodeEscape(std::string_view text, std::size_t pos, std::string& out) {
    std::optional<unsigned> const first = hex4(text, pos + 2);
    if (!first) {
        return std::nullopt;
    }
    if (*first >= 0xD800 && *first < 0xDC00 && pos + 12 <= text.size() && text.at(pos + 6) == '\\' &&
        text.at(pos + 7) == 'u') {
        std::optional<unsigned> const second = hex4(text, pos + 8);
        if (second && *second >= 0xDC00 && *second < 0xE000) {
            appendUtf8(out, 0x10000 + ((*first - 0xD800) << 10) + (*second - 0xDC00));
            return 12;
        }
    }
    appendUtf8(out, *first);
    return 6;
}

// A string at its opening quote: the offset past its closing quote. Its content is appended to @p out when one is
// given, escapes decoded, `\u` ones to UTF-8.
std::optional<std::size_t> scanString(std::string_view text, std::size_t pos, std::string* out) {
    if (pos >= text.size() || text.at(pos) != '"') {
        return std::nullopt;
    }
    for (++pos; pos < text.size();) {
        char const byte = text.at(pos);
        if (byte == '"') {
            return pos + 1;
        }
        if (byte != '\\') {
            if (out != nullptr) {
                out->push_back(byte);
            }
            ++pos;
            continue;
        }
        if (pos + 1 >= text.size()) {
            return std::nullopt;
        }
        if (text.at(pos + 1) == 'u') {
            std::optional<std::size_t> const used =
                out != nullptr ? appendUnicodeEscape(text, pos, *out) : skipUnicodeEscape(text, pos);
            if (!used) {
                return std::nullopt;
            }
            pos += *used;
            continue;
        }
        if (out != nullptr) {
            out->push_back(unescaped(text.at(pos + 1)));
        }
        pos += 2;
    }
    return std::nullopt;
}

std::optional<std::size_t> skipValue(std::string_view text, std::size_t pos, std::size_t depth);

// The container at @p pos, skipped: the offset past its closing bracket.
// NOLINTNEXTLINE(misc-no-recursion): a container's values, bounded by kMaxJsonDepth in skipValue.
std::optional<std::size_t> skipContainer(std::string_view text, std::size_t pos, std::size_t depth) {
    char const close = text.at(pos) == '{' ? '}' : ']';
    pos = skipSpace(text, pos + 1);
    if (pos < text.size() && text.at(pos) == close) {
        return pos + 1;
    }
    while (pos < text.size()) {
        if (close == '}') {
            std::optional<std::size_t> const name = scanString(text, pos, nullptr);
            if (!name) {
                return std::nullopt;
            }
            pos = skipSpace(text, *name);
            if (pos >= text.size() || text.at(pos) != ':') {
                return std::nullopt;
            }
            pos = skipSpace(text, pos + 1);
        }
        std::optional<std::size_t> const value = skipValue(text, pos, depth + 1);
        if (!value) {
            return std::nullopt;
        }
        pos = skipSpace(text, *value);
        if (pos < text.size() && text.at(pos) == ',') {
            pos = skipSpace(text, pos + 1);
            continue;
        }
        if (pos < text.size() && text.at(pos) == close) {
            return pos + 1;
        }
        return std::nullopt;
    }
    return std::nullopt;
}

// Any value at @p pos, skipped: the offset past it.
// NOLINTNEXTLINE(misc-no-recursion): a container's values, bounded by kMaxJsonDepth.
std::optional<std::size_t> skipValue(std::string_view text, std::size_t pos, std::size_t depth) {
    if (depth > kMaxJsonDepth || pos >= text.size()) {
        return std::nullopt;
    }
    char const first = text.at(pos);
    if (first == '"') {
        return scanString(text, pos, nullptr);
    }
    if (first == '{' || first == '[') {
        return skipContainer(text, pos, depth);
    }
    // A number or a literal runs to the next delimiter.
    std::size_t end = pos;
    while (end < text.size() && !isSpace(text.at(end)) && text.at(end) != ',' && text.at(end) != '}' &&
           text.at(end) != ']') {
        ++end;
    }
    return end == pos ? std::nullopt : std::optional<std::size_t>{end};
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
    for (std::size_t pos = 1; pos <= pointer.size(); ++pos) {
        if (pos == pointer.size() || pointer.at(pos) == '/') {
            tokens.push_back(std::move(token));
            token.clear();
            continue;
        }
        if (pointer.at(pos) == '~') {
            if (pos + 1 >= pointer.size() || (pointer.at(pos + 1) != '0' && pointer.at(pos + 1) != '1')) {
                return std::nullopt;
            }
            token.push_back(pointer.at(pos + 1) == '0' ? '~' : '/');
            ++pos;
            continue;
        }
        token.push_back(pointer.at(pos));
    }
    return tokens;
}

// An array index token: digits, with no leading zero but for `0` itself.
std::optional<std::size_t> indexOf(std::string const& token) {
    if (token.empty() || (token.size() > 1 && token.front() == '0')) {
        return std::nullopt;
    }
    std::size_t index = 0;
    auto const [end, error] = std::from_chars(token.data(), std::to_address(token.end()), index);
    if (error != std::errc{} || end != std::to_address(token.end())) {
        return std::nullopt;
    }
    return index;
}

}  // namespace

namespace {

// How wide a line of the source view may be before a container that would fit on it is broken up.
constexpr std::size_t kSourceColumns = 100;
constexpr std::size_t kSourceIndent = 2;

// Writes valid JSON text with members in their order and every scalar as it was written, one container per line unless
// the whole container fits in `kSourceColumns` columns. The text must already be valid JSON nested within
// `kMaxJsonDepth`, which `prettyJson` checks.
class SourcePrinter {
public:
    explicit SourcePrinter(std::string_view text) : _text{text} {}

    std::string print() {
        std::size_t const start = skipSpace(_text, 0);
        _out.reserve(_text.size());
        write(start, {.indent = 0, .column = 0, .depth = 0});
        return std::move(_out);
    }

private:
    // Where a value is written: nested @p indent columns in, starting at @p column on its line, @p depth containers
    // down.
    struct Placement {
        std::size_t indent;
        std::size_t column;
        std::size_t depth;
    };

    // @p pos starts a value, written at @p where. Answers where the value ends in the text.
    // NOLINTNEXTLINE(misc-no-recursion): bounded by kMaxJsonDepth, which prettyJson checks first.
    std::size_t write(std::size_t pos, Placement const& where) {
        auto const [indent, column, depth] = where;
        char const first = _text.at(pos);
        if (first != '{' && first != '[') {
            std::size_t const end = skipValue(_text, pos, depth).value_or(_text.size());
            _out.append(_text.substr(pos, end - pos));
            return end;
        }
        std::string line;
        // Room for the container and a comma after it.
        std::size_t const room = column + 1 < kSourceColumns ? kSourceColumns - column - 1 : 0;
        if (std::optional<std::size_t> const end = oneLine(pos, line, room, depth)) {
            _out += line;
            return *end;
        }
        char const close = first == '{' ? '}' : ']';
        _out.push_back(first);
        std::size_t cursor = skipSpace(_text, pos + 1);
        bool firstMember = true;
        while (_text.at(cursor) != close) {
            _out += firstMember ? "\n" : ",\n";
            firstMember = false;
            std::size_t const inner = indent + kSourceIndent;
            _out.append(inner, ' ');
            std::size_t shown = inner;
            if (first == '{') {
                std::size_t const nameEnd = scanString(_text, cursor, nullptr).value_or(_text.size());
                _out.append(_text.substr(cursor, nameEnd - cursor));
                _out += ": ";
                shown += nameEnd - cursor + 2;
                cursor = skipSpace(_text, skipSpace(_text, nameEnd) + 1);
            }
            cursor = skipSpace(_text, write(cursor, {.indent = inner, .column = shown, .depth = depth + 1}));
            if (_text.at(cursor) == ',') {
                cursor = skipSpace(_text, cursor + 1);
            }
        }
        _out.push_back('\n');
        _out.append(indent, ' ');
        _out.push_back(close);
        return cursor + 1;
    }

    // The value at @p pos on one line, `, ` and `: ` between its parts, appended to @p line; where it ends in the
    // text, or nothing when the line would be longer than @p room bytes.
    // NOLINTNEXTLINE(misc-no-recursion): bounded by kMaxJsonDepth, which prettyJson checks first.
    [[nodiscard]] std::optional<std::size_t> oneLine(std::size_t pos, std::string& line, std::size_t room,
                                                     std::size_t depth) const {
        char const first = _text.at(pos);
        if (first != '{' && first != '[') {
            std::size_t const end = skipValue(_text, pos, depth).value_or(_text.size());
            line.append(_text.substr(pos, end - pos));
            return line.size() <= room ? std::optional<std::size_t>{end} : std::nullopt;
        }
        char const close = first == '{' ? '}' : ']';
        line.push_back(first);
        std::size_t cursor = skipSpace(_text, pos + 1);
        bool firstMember = true;
        while (_text.at(cursor) != close) {
            if (!firstMember) {
                line += ", ";
            }
            firstMember = false;
            if (first == '{') {
                std::size_t const nameEnd = scanString(_text, cursor, nullptr).value_or(_text.size());
                line.append(_text.substr(cursor, nameEnd - cursor));
                line += ": ";
                cursor = skipSpace(_text, skipSpace(_text, nameEnd) + 1);
            }
            std::optional<std::size_t> const end = oneLine(cursor, line, room, depth + 1);
            if (!end) {
                return std::nullopt;
            }
            cursor = skipSpace(_text, *end);
            if (_text.at(cursor) == ',') {
                cursor = skipSpace(_text, cursor + 1);
            }
        }
        line.push_back(close);
        return line.size() <= room ? std::optional<std::size_t>{cursor + 1} : std::nullopt;
    }

    std::string_view _text;
    std::string _out;
};

}  // namespace

std::string prettyJson(std::string_view text) {
    if (!parseJson(text)) {
        return std::string{text};
    }
    return SourcePrinter{text}.print();
}

std::string compactJson(std::string_view text) {
    if (!parseJson(text)) {
        return std::string{text};
    }
    std::string out;
    out.reserve(text.size());
    bool inString = false;
    bool escaped = false;
    for (char const character : text) {
        if (inString) {
            out.push_back(character);
            if (escaped) {
                escaped = false;
            } else if (character == '\\') {
                escaped = true;
            } else if (character == '"') {
                inString = false;
            }
        } else if (character == '"') {
            inString = true;
            out.push_back(character);
        } else if (character != ' ' && character != '\t' && character != '\n' && character != '\r') {
            out.push_back(character);
        }
    }
    return out;
}

// One walk over the document per pointer token, and the text and the pointer are two strings the caller names by role.
// NOLINTNEXTLINE(readability-function-cognitive-complexity,bugprone-easily-swappable-parameters)
std::optional<std::size_t> pointerOffset(std::string_view text, std::string_view pointer) {
    std::optional<std::vector<std::string>> const tokens = tokensOf(pointer);
    if (!tokens) {
        return std::nullopt;
    }
    std::size_t value = skipSpace(text, 0);  // where the value reached so far starts
    std::size_t shown = value;               // where a person sees it: its member's name, or the value itself
    for (std::size_t depth = 0; depth < tokens->size(); ++depth) {
        if (depth >= kMaxJsonDepth || value >= text.size() || (text.at(value) != '{' && text.at(value) != '[')) {
            return std::nullopt;
        }
        std::string const& token = tokens->at(depth);
        bool const object = text.at(value) == '{';
        std::optional<std::size_t> const wanted = object ? std::nullopt : indexOf(token);
        if (!object && !wanted) {
            return std::nullopt;
        }
        std::size_t pos = skipSpace(text, value + 1);
        std::size_t place = 0;
        bool found = false;
        while (pos < text.size() && text.at(pos) != (object ? '}' : ']')) {
            std::size_t const entry = pos;
            bool matches = false;
            if (object) {
                std::string name;
                std::optional<std::size_t> const after = scanString(text, pos, &name);
                if (after) {
                    pos = skipSpace(text, *after);
                }
                if (!after || pos >= text.size() || text.at(pos) != ':') {
                    if (found) {
                        break;
                    }
                    return std::nullopt;
                }
                pos = skipSpace(text, pos + 1);
                matches = name == token;
            } else {
                matches = place == *wanted;
            }
            if (matches) {
                if (pos >= text.size() || text.at(pos) == ',' || text.at(pos) == '}' || text.at(pos) == ']') {
                    return std::nullopt;
                }
                // The last of equal names wins, as it does in the parsed value, so the scan goes on.
                value = pos;
                shown = entry;
                found = true;
            }
            // What follows the match may be cut short or malformed: the match so far stands.
            std::optional<std::size_t> const skipped = skipValue(text, pos, depth + 1);
            if (!skipped) {
                if (found) {
                    break;
                }
                return std::nullopt;
            }
            pos = skipSpace(text, *skipped);
            if (pos < text.size() && text.at(pos) == ',') {
                pos = skipSpace(text, pos + 1);
                if (pos < text.size() && text.at(pos) != (object ? '}' : ']')) {
                    ++place;
                    continue;
                }
            } else if (pos < text.size() && text.at(pos) == (object ? '}' : ']')) {
                break;
            }
            if (found) {
                break;
            }
            return std::nullopt;
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

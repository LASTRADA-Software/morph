// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "../util/datetime.hpp"

/// @file
/// @brief Date patterns: the one form in which an application shows and reads dates, such as `dd-MMM-yyyy HH:mm`.
///        The expression formatter, the interpreter and both frontends call these, so the grammar lives in one place.
///        Specified in `docs/spec/ui/expressions.md`.

namespace morph::ui {

namespace detail::dates {

/// @brief A token of a date pattern.
enum class Token : std::uint8_t {
    Year,       ///< `yyyy`: four digits.
    Month,      ///< `MM`: two digits.
    MonthName,  ///< `MMM`: `Jan` to `Dec`, in English, read without regard to case.
    Day,        ///< `dd`: two digits.
    Hour,       ///< `HH`: two digits, 00 to 23.
    Minute,     ///< `mm`: two digits.
    Second,     ///< `ss`: two digits.
    Offset,     ///< `xxx`: the zone's offset from UTC, `+02:00`.
    Literal,    ///< One of `-`, `/`, `.`, `:`, `,` and a space.
};

/// @brief One element of a parsed pattern.
struct Element {
    /// @brief What it is.
    Token token = Token::Literal;
    /// @brief The character of a literal.
    char literal = ' ';
};

/// @brief The English month abbreviations `MMM` writes and reads.
inline constexpr std::array<std::string_view, 12> kMonths = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                                             "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

/// @brief A run of one letter as a token: its letter and its length.
/// @param letter The letter.
/// @param count  How many times it repeats.
/// @return The token; `nullopt` for a run that is none.
[[nodiscard]] inline std::optional<Token> tokenOf(char letter, std::size_t count) noexcept {
    if (letter == 'y' && count == 4) {
        return Token::Year;
    }
    if (letter == 'M' && count == 2) {
        return Token::Month;
    }
    if (letter == 'M' && count == 3) {
        return Token::MonthName;
    }
    if (letter == 'x' && count == 3) {
        return Token::Offset;
    }
    if (count != 2) {
        return std::nullopt;
    }
    switch (letter) {
        case 'd':
            return Token::Day;
        case 'H':
            return Token::Hour;
        case 'm':
            return Token::Minute;
        case 's':
            return Token::Second;
        default:
            return std::nullopt;
    }
}

/// @brief A pattern split into its tokens and literals.
/// @param pattern The pattern.
/// @return The elements; `nullopt` when it holds anything but the tokens and the literals.
[[nodiscard]] inline std::optional<std::vector<Element>> elementsOf(std::string_view pattern) {
    std::vector<Element> elements;
    for (std::size_t cursor = 0; cursor < pattern.size();) {
        char const character = pattern.at(cursor);
        if (std::string_view{"-/.:, "}.contains(character)) {
            elements.push_back(Element{.token = Token::Literal, .literal = character});
            ++cursor;
            continue;
        }
        std::size_t run = cursor;
        while (run < pattern.size() && pattern.at(run) == character) {
            ++run;
        }
        std::optional<Token> const token = tokenOf(character, run - cursor);
        if (!token) {
            return std::nullopt;
        }
        elements.push_back(Element{.token = *token});
        cursor = run;
    }
    return elements;
}

/// @brief Whether a token is written as digits.
/// @param token The token.
/// @return True for every token but `MMM`, `xxx` and a literal.
[[nodiscard]] constexpr bool numeric(Token token) noexcept {
    return token != Token::MonthName && token != Token::Offset && token != Token::Literal;
}

/// @brief Reads @p count digits of @p text cursor @p cursor.
/// @param text  The text.
/// @param cursor    Where to read; moved past the digits.
/// @param count How many digits.
/// @return The number; `nullopt` when there are not that many digits.
[[nodiscard]] inline std::optional<int> digits(std::string_view text, std::size_t& cursor, std::size_t count) {
    if (cursor + count > text.size()) {
        return std::nullopt;
    }
    int value = 0;
    for (std::size_t index = 0; index < count; ++index) {
        char const digit = text.at(cursor + index);
        if (digit < '0' || digit > '9') {
            return std::nullopt;
        }
        value = (value * 10) + (digit - '0');
    }
    cursor += count;
    return value;
}

/// @brief The fields a pattern reads out of a text.
struct Fields {
    int year = 1970;            ///< `yyyy`.
    unsigned month = 1;         ///< `MM` or `MMM`.
    unsigned day = 1;           ///< `dd`.
    int hour = 0;               ///< `HH`.
    int minute = 0;             ///< `mm`.
    int second = 0;             ///< `ss`.
    std::optional<int> offset;  ///< `xxx`, in minutes.
};

/// @brief Reads `MMM` out of @p text cursor @p cursor, without regard to case.
/// @param text The text.
/// @param cursor   Where to read; moved past it.
/// @return The month, 1 to 12; `nullopt` when no month's name is there.
[[nodiscard]] inline std::optional<unsigned> monthName(std::string_view text, std::size_t& cursor) {
    if (cursor + 3 > text.size()) {
        return std::nullopt;
    }
    auto const lower = [](char letter) { return std::tolower(static_cast<unsigned char>(letter)); };
    for (std::size_t month = 0; month < kMonths.size(); ++month) {
        std::string_view const name = kMonths.at(month);
        if (lower(text.at(cursor)) == lower(name.at(0)) && lower(text.at(cursor + 1)) == lower(name.at(1)) &&
            lower(text.at(cursor + 2)) == lower(name.at(2))) {
            cursor += 3;
            return static_cast<unsigned>(month + 1);
        }
    }
    return std::nullopt;
}

/// @brief Reads `xxx` out of @p text cursor @p cursor: a sign, two hour digits, `:` and two minute digits.
/// @param text The text.
/// @param cursor   Where to read; moved past it.
/// @return The offset from UTC in minutes; `nullopt` when none is there.
[[nodiscard]] inline std::optional<int> offset(std::string_view text, std::size_t& cursor) {
    if (cursor >= text.size() || (text.at(cursor) != '+' && text.at(cursor) != '-')) {
        return std::nullopt;
    }
    int const sign = text.at(cursor) == '-' ? -1 : 1;
    ++cursor;
    std::optional<int> const hours = digits(text, cursor, 2);
    if (!hours || cursor >= text.size() || text.at(cursor) != ':') {
        return std::nullopt;
    }
    ++cursor;
    std::optional<int> const minutes = digits(text, cursor, 2);
    if (!minutes || *hours > 23 || *minutes > 59) {
        return std::nullopt;
    }
    return sign * ((*hours * 60) + *minutes);
}

/// @brief Reads one element of a pattern out of @p text cursor @p cursor into @p fields.
/// @param element The element.
/// @param text    The text.
/// @param cursor      Where to read; moved past it.
/// @param fields  Where the value goes.
/// @return Whether the text holds the element there.
[[nodiscard]] inline bool readElement(Element const& element, std::string_view text, std::size_t& cursor, Fields& fields) {
    if (element.token == Token::Literal) {
        if (cursor >= text.size() || text.at(cursor) != element.literal) {
            return false;
        }
        ++cursor;
        return true;
    }
    if (element.token == Token::MonthName) {
        std::optional<unsigned> const month = monthName(text, cursor);
        fields.month = month.value_or(0);
        return month.has_value();
    }
    if (element.token == Token::Offset) {
        fields.offset = offset(text, cursor);
        return fields.offset.has_value();
    }
    std::optional<int> const value = digits(text, cursor, element.token == Token::Year ? 4 : 2);
    if (!value) {
        return false;
    }
    if (element.token == Token::Year) {
        fields.year = *value;
    } else if (element.token == Token::Month) {
        fields.month = static_cast<unsigned>(*value);
    } else if (element.token == Token::Day) {
        fields.day = static_cast<unsigned>(*value);
    } else if (element.token == Token::Hour) {
        fields.hour = *value;
    } else if (element.token == Token::Minute) {
        fields.minute = *value;
    } else {
        fields.second = *value;
    }
    return true;
}

/// @brief The instant a text names in a pattern's elements.
/// @param elements      The pattern.
/// @param text          The text.
/// @param offsetMinutes The display zone's offset, used when the pattern holds no `xxx`.
/// @return The UTC instant; `nullopt` for text that names none, a day that does not exist among them.
[[nodiscard]] inline std::optional<time::DateTime> read(std::vector<Element> const& elements, std::string_view text,
                                                        std::int32_t offsetMinutes) {
    Fields fields;
    std::size_t cursor = 0;
    for (Element const& element : elements) {
        if (!readElement(element, text, cursor, fields)) {
            return std::nullopt;
        }
    }
    std::chrono::year_month_day const date{std::chrono::year{fields.year}, std::chrono::month{fields.month},
                                           std::chrono::day{fields.day}};
    if (cursor != text.size() || !date.ok() || fields.hour > 23 || fields.minute > 59 || fields.second > 59) {
        return std::nullopt;
    }
    auto const local = std::chrono::sys_days{date} + std::chrono::hours{fields.hour} +
                       std::chrono::minutes{fields.minute} + std::chrono::seconds{fields.second};
    auto const utc = local - std::chrono::minutes{fields.offset.value_or(offsetMinutes)};
    return time::DateTime{std::chrono::time_point_cast<std::chrono::milliseconds>(utc)};
}

/// @brief @p value as two digits, or four for a year.
/// @param value The number.
/// @param width How many digits.
/// @return The digits, zero-padded.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a number, then how wide to write it.
[[nodiscard]] inline std::string padded(long long value, std::size_t width) {
    std::string text = std::to_string(value < 0 ? -value : value);
    if (text.size() < width) {
        text.insert(0, width - text.size(), '0');
    }
    return value < 0 ? "-" + text : text;
}

}  // namespace detail::dates

/// @brief Checks a date pattern: only the tokens `yyyy`, `MM`, `MMM`, `dd`, `HH`, `mm`, `ss` and `xxx` and the
///        literals `-`, `/`, `.`, `:`, `,` and a space; `yyyy`, `dd` and one of `MM` or `MMM` once each; for a date
///        and time also `HH` and `mm` once each and `ss` and `xxx` cursor most once, and for a date no time token; and no
///        two tokens touching without a literal between them, except `MMM` beside a number. So every text it writes
///        reads back to the same instant.
/// @param pattern  The pattern, such as `dd-MMM-yyyy HH:mm`.
/// @param withTime Whether it is a date-and-time pattern rather than a date pattern.
/// @return Nothing, or why the pattern is refused.
[[nodiscard]] inline std::expected<void, std::string> checkDatePattern(std::string_view pattern, bool withTime) {
    using detail::dates::Token;
    auto const elements = detail::dates::elementsOf(pattern);
    if (!elements) {
        return std::unexpected{
            "a date pattern holds only yyyy, MM, MMM, dd, HH, mm, ss, xxx and the literals - / . : , "
            "and a space"};
    }
    std::array<int, 9> counts{};
    for (std::size_t index = 0; index < elements->size(); ++index) {
        Token const token = elements->at(index).token;
        ++counts.at(static_cast<std::size_t>(token));
        if (index > 0 && token != Token::Literal) {
            Token const before = elements->at(index - 1).token;
            bool const touching = before != Token::Literal;
            bool const nameBesideNumber = (token == Token::MonthName && detail::dates::numeric(before)) ||
                                          (before == Token::MonthName && detail::dates::numeric(token));
            if (touching && !nameBesideNumber) {
                return std::unexpected{"two tokens of a date pattern touch without a literal between them"};
            }
        }
    }
    auto const count = [&counts](Token token) { return counts.at(static_cast<std::size_t>(token)); };
    if (count(Token::Year) != 1 || count(Token::Day) != 1 || count(Token::Month) + count(Token::MonthName) != 1) {
        return std::unexpected{"a date pattern holds yyyy, dd, and MM or MMM, each once"};
    }
    bool const anyTime = count(Token::Hour) + count(Token::Minute) + count(Token::Second) + count(Token::Offset) > 0;
    if (!withTime && anyTime) {
        return std::unexpected{"a date pattern holds no time token"};
    }
    if (withTime && (count(Token::Hour) != 1 || count(Token::Minute) != 1 || count(Token::Second) > 1 ||
                     count(Token::Offset) > 1)) {
        return std::unexpected{"a date-and-time pattern holds HH and mm once each, and ss and xxx cursor most once"};
    }
    return {};
}

/// @brief Writes an instant in a pattern.
/// @param pattern       A pattern `checkDatePattern` takes.
/// @param local         The instant, already moved into the display zone.
/// @param offsetMinutes The display zone's offset from UTC, which `xxx` writes as `+02:00` (`+00:00` for UTC).
/// @return The text, such as `24-Oct-2026 09:41`; empty for a pattern that is not one.
[[nodiscard]] inline std::string formatDate(std::string_view pattern, time::DateTime local,
                                            std::int32_t offsetMinutes) {
    using detail::dates::Token;
    auto const elements = detail::dates::elementsOf(pattern);
    if (!elements) {
        return {};
    }
    auto const day = std::chrono::floor<std::chrono::days>(local.value);
    std::chrono::year_month_day const date{day};
    std::chrono::hh_mm_ss const clock{std::chrono::floor<std::chrono::seconds>(local.value - day)};
    std::string text;
    for (detail::dates::Element const& element : *elements) {
        switch (element.token) {
            case Token::Literal:
                text += element.literal;
                break;
            case Token::Year:
                text += detail::dates::padded(static_cast<int>(date.year()), 4);
                break;
            case Token::Month:
                text += detail::dates::padded(static_cast<unsigned>(date.month()), 2);
                break;
            case Token::MonthName:
                text += detail::dates::kMonths.at(static_cast<unsigned>(date.month()) - 1);
                break;
            case Token::Day:
                text += detail::dates::padded(static_cast<unsigned>(date.day()), 2);
                break;
            case Token::Hour:
                text += detail::dates::padded(clock.hours().count(), 2);
                break;
            case Token::Minute:
                text += detail::dates::padded(clock.minutes().count(), 2);
                break;
            case Token::Second:
                text += detail::dates::padded(clock.seconds().count(), 2);
                break;
            case Token::Offset:
            default: {
                std::int32_t const minutes = offsetMinutes < 0 ? -offsetMinutes : offsetMinutes;
                text += (offsetMinutes < 0 ? "-" : "+") + detail::dates::padded(minutes / 60, 2) + ":" +
                        detail::dates::padded(minutes % 60, 2);
                break;
            }
        }
    }
    return text;
}

/// @brief Reads the instant a text names: in the pattern, then as ISO 8601 (`yyyy-MM-dd`, `yyyy-MM-dd HH:mm` or
///        `yyyy-MM-ddTHH:mm` in the display zone, or a full instant such as `2026-10-24T09:41:00Z`), so a pasted ISO
///        date still works. A pattern with `xxx` takes the instant from the offset the text names; any other reads it
///        in the display zone.
/// @param pattern       The pattern; empty reads ISO only.
/// @param text          The text.
/// @param offsetMinutes The display zone's offset from UTC.
/// @return The UTC instant; `nullopt` for text that names none, such as `31-Feb-2026`.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the pattern, then the text, as `formatDate` takes them.
[[nodiscard]] inline std::optional<time::DateTime> parseDate(std::string_view pattern, std::string_view text,
                                                             std::int32_t offsetMinutes) {
    for (std::string_view const form : {pattern, std::string_view{"yyyy-MM-dd HH:mm"},
                                        std::string_view{"yyyy-MM-ddTHH:mm"}, std::string_view{"yyyy-MM-dd"}}) {
        if (form.empty()) {
            continue;
        }
        std::optional<std::vector<detail::dates::Element>> elements;
        if (form == "yyyy-MM-ddTHH:mm") {
            elements = detail::dates::elementsOf("yyyy-MM-dd HH:mm");
            if (elements) {
                elements->at(5).literal = 'T';  // the space between the date and the clock
            }
        } else {
            elements = detail::dates::elementsOf(form);
        }
        if (elements) {
            if (auto instant = detail::dates::read(*elements, text, offsetMinutes)) {
                return instant;
            }
        }
    }
    return time::DateTime::fromIso8601(text);
}

}  // namespace morph::ui

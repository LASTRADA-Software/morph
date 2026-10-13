// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file forms/engine/detail/text.hpp
/// @brief Exact text helpers the forms engine shares: JSON quoting and reading, canonical
///        decimal comparison and digit arithmetic, decimal rendering of a `Rational`, and
///        ISO-8601 wall-clock conversion.
///
/// Specified in `docs/spec/forms/engine.md`.

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <format>
#include <glaze/glaze.hpp>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "../../../ui/value.hpp"
#include "../../../util/rational.hpp"

/// @brief Implementation details of the forms engine (`include/morph/forms/engine/`).
namespace morph::forms::detail::engine {

/// @brief The JSON DOM the engine reads: integers keep every digit, as `uint64_t` or `int64_t`.
using Json = glz::generic_u64;

/// @brief Strips ASCII whitespace from both ends.
/// @param text The text.
/// @return A view of @p text without leading or trailing spaces, tabs or line breaks.
[[nodiscard]] inline std::string_view trim(std::string_view text) noexcept {
    constexpr std::string_view kSpace = " \t\r\n\f\v";
    auto const first = text.find_first_not_of(kSpace);
    if (first == std::string_view::npos) {
        return {};
    }
    auto const last = text.find_last_not_of(kSpace);
    return text.substr(first, last - first + 1);
}

/// @brief Quotes @p text as a JSON string exactly as `JSON.stringify` does: `"` and `\` escaped,
///        the five short control escapes, any other control character as `\u00xx`, everything
///        else verbatim. The body bytes the engine produces depend on this spelling.
/// @param text UTF-8 text.
/// @return The quoted literal.
[[nodiscard]] inline std::string quote(std::string_view text) {
    constexpr std::string_view kHex = "0123456789abcdef";
    std::string out;
    out.reserve(text.size() + 2);
    out += '"';
    for (char const chr : text) {
        auto const code = static_cast<unsigned char>(chr);
        switch (chr) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (code < 0x20U) {
                    out += "\\u00";
                    out += kHex.at(code >> 4U);
                    out += kHex.at(code & 0x0FU);
                } else {
                    out += chr;
                }
                break;
        }
    }
    out += '"';
    return out;
}

/// @brief Parses JSON text into the u64-mode DOM, refusing text nested deeper than `ui::kMaxJsonDepth`: a schema
///        or a reply is served text, and every pass over the tree recurses.
/// @param text JSON text.
/// @return The DOM, or `std::nullopt` when @p text is not JSON or nests too deeply.
[[nodiscard]] inline std::optional<Json> parseJson(std::string_view text) {
    auto parsed = ui::parseJson(text);
    if (!parsed) {
        return std::nullopt;
    }
    return *std::move(parsed);
}

/// @brief Writes a DOM value as compact JSON; integers keep every digit.
/// @param value The value.
/// @return Its JSON text.
[[nodiscard]] inline std::string toJson(Json const& value) { return glz::write_json(value).value_or(std::string{}); }

/// @brief A DOM node holding an empty JSON object.
/// @return `{}`.
[[nodiscard]] inline Json emptyObject() {
    Json node{};
    node.data.emplace<Json::object_t>();
    return node;
}

/// @brief The member @p key of an object node, without inserting anything.
/// @param node The node; a non-object has no members.
/// @param key  The member name.
/// @return The member, or null.
[[nodiscard]] inline Json const* member(Json const& node, std::string_view key) {
    auto const* const object = node.get_if<Json::object_t>();
    if (object == nullptr) {
        return nullptr;
    }
    const auto* const found = object->find(key);
    return found == object->end() ? nullptr : &found->second;
}

/// @brief The string member @p key of an object node.
/// @param node The node.
/// @param key  The member name.
/// @return The string, or null when absent or not a string.
[[nodiscard]] inline std::string const* stringAt(Json const& node, std::string_view key) {
    Json const* const found = member(node, key);
    return found == nullptr ? nullptr : found->get_if<std::string>();
}

/// @brief A JSON number as canonical decimal text, `-?[0-9]+(\.[0-9]+)?`.
///
/// Integers keep every digit; a double is written in fixed notation with the shortest digits
/// that read back as the same double, so `1e-7` is `"0.0000001"` and no text has an exponent.
/// @param value The value.
/// @return The text, or `std::nullopt` when @p value is not a finite number.
[[nodiscard]] inline std::optional<std::string> numberText(Json const& value) {
    if (auto const* const unsignedValue = value.get_if<std::uint64_t>()) {
        return std::to_string(*unsignedValue);
    }
    if (auto const* const signedValue = value.get_if<std::int64_t>()) {
        return std::to_string(*signedValue);
    }
    if (auto const* const doubleValue = value.get_if<double>()) {
        if (!std::isfinite(*doubleValue)) {
            return std::nullopt;
        }
        // 309 integer digits for DBL_MAX, a sign, and the longest shortest fraction.
        std::array<char, 400> buffer{};
        auto const result = std::to_chars(std::to_address(buffer.begin()), std::to_address(buffer.end()), *doubleValue,
                                          std::chars_format::fixed);
        if (result.ec != std::errc{}) {
            return std::nullopt;
        }
        std::string text{buffer.data(), result.ptr};
        if (text == "-0") {
            text = "0";
        }
        return text;
    }
    return std::nullopt;
}

/// @brief A scalar as display text: a string as itself, a number as `numberText`, a boolean
///        as `true`/`false`, null as empty, anything else as compact JSON.
/// @param value The value.
/// @return The text.
[[nodiscard]] inline std::string displayText(Json const& value) {
    if (auto const* const text = value.get_if<std::string>()) {
        return *text;
    }
    if (auto number = numberText(value)) {
        return *std::move(number);
    }
    if (auto const* const flag = value.get_if<bool>()) {
        return *flag ? "true" : "false";
    }
    if (value.is_null()) {
        return {};
    }
    return toJson(value);
}

/// @brief Whether @p text is `-?[0-9]+(\.[0-9]+)?`.
/// @param text The text.
/// @return `true` for canonical decimal text.
[[nodiscard]] inline bool isCanonicalDecimal(std::string_view text) noexcept {
    if (text.starts_with('-')) {
        text.remove_prefix(1);
    }
    auto const dot = text.find('.');
    auto const digits = [](std::string_view part) {
        return !part.empty() && part.find_first_not_of("0123456789") == std::string_view::npos;
    };
    if (dot == std::string_view::npos) {
        return digits(text);
    }
    return digits(text.substr(0, dot)) && digits(text.substr(dot + 1));
}

/// @brief Whether @p text is `-?[0-9]+`.
/// @param text The text.
/// @return `true` for canonical integer text.
[[nodiscard]] inline bool isCanonicalInteger(std::string_view text) noexcept {
    return isCanonicalDecimal(text) && !text.contains('.');
}

/// @brief Drops a leading run of zeros that JSON forbids: `"007.50"` → `"7.50"`, `"-00"` → `"-0"`.
/// @param canonical Canonical decimal text.
/// @return The text with at most one leading zero before the point.
[[nodiscard]] inline std::string stripLeadingZeros(std::string_view canonical) {
    std::string out;
    if (canonical.starts_with('-')) {
        out += '-';
        canonical.remove_prefix(1);
    }
    while (canonical.size() > 1 && canonical.front() == '0' && canonical.at(1) != '.') {
        canonical.remove_prefix(1);
    }
    out += canonical;
    return out;
}

/// @brief The ordering the other way round: `less` for `greater`, `greater` for `less`, `equal` for `equal`.
/// @param order The ordering.
/// @return Its reverse.
[[nodiscard]] constexpr std::strong_ordering reversed(std::strong_ordering order) noexcept {
    if (std::is_lt(order)) {
        return std::strong_ordering::greater;
    }
    if (std::is_gt(order)) {
        return std::strong_ordering::less;
    }
    return std::strong_ordering::equal;
}

/// @brief Orders two canonical decimals exactly, at any magnitude; `-0` equals `0`.
/// @param lhs Canonical decimal text (leading zeros allowed).
/// @param rhs Canonical decimal text (leading zeros allowed).
/// @return The ordering of the two values.
[[nodiscard]] inline std::strong_ordering compareDecimal(std::string_view lhs, std::string_view rhs) noexcept {
    struct Parts {
        bool negative = false;
        std::string_view whole;
        std::string_view fraction;
    };
    auto const split = [](std::string_view text) {
        Parts parts{};
        parts.negative = text.starts_with('-');
        if (parts.negative) {
            text.remove_prefix(1);
        }
        auto const dot = text.find('.');
        parts.whole = text.substr(0, dot);
        parts.fraction = dot == std::string_view::npos ? std::string_view{} : text.substr(dot + 1);
        while (parts.whole.size() > 1 && parts.whole.front() == '0') {
            parts.whole.remove_prefix(1);
        }
        while (!parts.fraction.empty() && parts.fraction.back() == '0') {
            parts.fraction.remove_suffix(1);
        }
        if ((parts.whole.empty() || parts.whole == "0") && parts.fraction.empty()) {
            parts.negative = false;
        }
        return parts;
    };
    auto const magnitude = [](Parts const& left, Parts const& right) {
        if (left.whole.size() != right.whole.size()) {
            return left.whole.size() <=> right.whole.size();
        }
        if (auto const order = left.whole <=> right.whole; std::is_neq(order)) {
            return order;
        }
        return left.fraction <=> right.fraction;
    };
    Parts const left = split(lhs);
    Parts const right = split(rhs);
    if (left.negative != right.negative) {
        return left.negative ? std::strong_ordering::less : std::strong_ordering::greater;
    }
    auto const order = magnitude(left, right);
    if (!left.negative) {
        return order;
    }
    return reversed(order);
}

/// @brief Multiplies two non-negative digit strings exactly.
/// @param lhs Decimal digits.
/// @param rhs Decimal digits.
/// @return The product's digits, without leading zeros.
[[nodiscard]] inline std::string multiplyDigits(std::string_view lhs, std::string_view rhs) {
    std::vector<std::uint32_t> product(lhs.size() + rhs.size(), 0U);
    for (std::size_t i = lhs.size(); i-- > 0;) {
        for (std::size_t j = rhs.size(); j-- > 0;) {
            auto const left = static_cast<std::uint32_t>(lhs.at(i) - '0');
            auto const right = static_cast<std::uint32_t>(rhs.at(j) - '0');
            std::uint32_t& low = product.at(i + j + 1);
            std::uint32_t const sum = (left * right) + low;
            low = sum % 10U;
            product.at(i + j) += sum / 10U;
        }
    }
    std::string out;
    for (std::uint32_t const digit : product) {
        if (out.empty() && digit == 0U) {
            continue;
        }
        out += static_cast<char>('0' + digit);
    }
    return out.empty() ? std::string{"0"} : out;
}

/// @brief Orders an exact rational against a canonical decimal, exactly, at any magnitude.
///
/// Cross-multiplies on digits: `|num| * 10^places` against `|digits| * den`, so neither side is
/// ever narrowed to a `Rational` or a `double`.
/// @param value   The rational.
/// @param decimal Canonical decimal text.
/// @return The ordering of @p value against @p decimal.
[[nodiscard]] inline std::strong_ordering compareRationalToDecimal(math::Rational const& value,
                                                                   std::string_view decimal) {
    bool const decimalNegative = decimal.starts_with('-');
    std::string_view const magnitude = decimalNegative ? decimal.substr(1) : decimal;
    auto const dot = magnitude.find('.');
    std::string digits{magnitude.substr(0, dot)};
    std::size_t places = 0;
    if (dot != std::string_view::npos) {
        digits += magnitude.substr(dot + 1);
        places = magnitude.size() - dot - 1;
    }
    bool const decimalZero = std::ranges::all_of(digits, [](char chr) { return chr == '0'; });
    auto const signOf = [](bool zero, bool negative) {
        if (zero) {
            return 0;
        }
        return negative ? -1 : 1;
    };
    int const decimalSign = signOf(decimalZero, decimalNegative);
    int const valueSign = signOf(value.numerator == 0, value.numerator < 0);
    if (valueSign != decimalSign) {
        return valueSign <=> decimalSign;
    }
    if (valueSign == 0) {
        return std::strong_ordering::equal;
    }
    std::string const scaledValue = std::to_string(math::detail::absU64(value.numerator)) + std::string(places, '0');
    std::string const scaledDecimal = multiplyDigits(digits, std::to_string(value.denominator));
    auto const order = compareDecimal(scaledValue, scaledDecimal);
    return valueSign > 0 ? order : reversed(order);
}

/// @brief Renders @p value as canonical decimal text, rounded half away from zero to @p places.
/// @param value  The value.
/// @param places Fraction digits; clamped to `math::kMaxDecimalPlaces`.
/// @return E.g. `"-0.33"` for `-1/3` at two places; a value that rounds to zero has no sign.
[[nodiscard]] inline std::string decimalText(math::Rational const& value, std::uint32_t places) {
    std::uint32_t const wanted = std::min(places, math::kMaxDecimalPlaces);
    math::Rational const rounded =
        math::roundToDecimalPlaces(value, math::DecimalPlaces{wanted}, math::RoundingMode::HalfAwayFromZero);
    std::int64_t const scale = math::detail::powerOfTen(wanted);
    std::string digits = multiplyDigits(std::to_string(math::detail::absU64(rounded.numerator)),
                                        std::to_string(scale / rounded.denominator));
    if (digits.size() <= wanted) {
        digits.insert(0, wanted + 1 - digits.size(), '0');
    }
    std::string out = rounded.numerator < 0 ? "-" : "";
    out += std::string_view{digits}.substr(0, digits.size() - wanted);
    if (wanted > 0) {
        out += '.';
        out += std::string_view{digits}.substr(digits.size() - wanted);
    }
    return out;
}

/// @brief Reads `count` ASCII digits at @p offset of @p text.
/// @param text   The text.
/// @param offset Where the digits start.
/// @param count  How many digits.
/// @return The value, or `std::nullopt` when a character there is not a digit.
[[nodiscard]] inline std::optional<int> digitsAt(std::string_view text, std::size_t offset, std::size_t count) {
    if (offset + count > text.size()) {
        return std::nullopt;
    }
    int value = 0;
    for (std::size_t i = offset; i < offset + count; ++i) {
        char const chr = text.at(i);
        if (chr < '0' || chr > '9') {
            return std::nullopt;
        }
        value = (value * 10) + (chr - '0');
    }
    return value;
}

/// @brief Reads `YYYY-MM-DDTHH:MM[:SS]` as a calendar instant, validating every component.
///
/// The result is the reading as if it were UTC; the caller shifts it by its display zone.
/// @param text The wall-clock text.
/// @return The instant, or `std::nullopt` for malformed or impossible text.
[[nodiscard]] inline std::optional<std::chrono::sys_seconds> parseWallClock(std::string_view text) {
    if (text.size() != 16 && text.size() != 19) {
        return std::nullopt;
    }
    if (text.at(4) != '-' || text.at(7) != '-' || text.at(10) != 'T' || text.at(13) != ':' ||
        (text.size() == 19 && text.at(16) != ':')) {
        return std::nullopt;
    }
    auto const year = digitsAt(text, 0, 4);
    auto const month = digitsAt(text, 5, 2);
    auto const day = digitsAt(text, 8, 2);
    auto const hour = digitsAt(text, 11, 2);
    auto const minute = digitsAt(text, 14, 2);
    auto const second = text.size() == 19 ? digitsAt(text, 17, 2) : std::optional<int>{0};
    if (!year || !month || !day || !hour || !minute || !second || *hour > 23 || *minute > 59 || *second > 59) {
        return std::nullopt;
    }
    std::chrono::year_month_day const date{std::chrono::year{*year}, std::chrono::month{static_cast<unsigned>(*month)},
                                           std::chrono::day{static_cast<unsigned>(*day)}};
    if (!date.ok()) {
        return std::nullopt;
    }
    return std::chrono::sys_days{date} + std::chrono::hours{*hour} + std::chrono::minutes{*minute} +
           std::chrono::seconds{*second};
}

/// @brief Reads an ISO-8601 instant: `YYYY-MM-DDTHH:MM[:SS[.fraction]][Z|±HH:MM|±HHMM]`.
///
/// No zone designator reads as UTC, which is what a `Timestamp` serialises to; a fraction is
/// dropped, since every engine display is to the second.
/// @param text The instant text.
/// @return The UTC instant, or `std::nullopt` when malformed.
[[nodiscard]] inline std::optional<std::chrono::sys_seconds> parseInstant(std::string_view text) {
    std::size_t clockEnd = 19;
    if (text.size() < 19 || text.at(16) != ':') {
        clockEnd = 16;
    }
    auto local = parseWallClock(text.substr(0, clockEnd));
    if (!local) {
        return std::nullopt;
    }
    std::string_view rest = text.substr(clockEnd);
    if (rest.starts_with('.')) {
        rest.remove_prefix(1);
        while (!rest.empty() && rest.front() >= '0' && rest.front() <= '9') {
            rest.remove_prefix(1);
        }
    }
    if (rest.empty() || rest == "Z") {
        return local;
    }
    if (rest.front() != '+' && rest.front() != '-') {
        return std::nullopt;
    }
    int const sign = rest.front() == '-' ? -1 : 1;
    rest.remove_prefix(1);
    std::string compact{rest};
    if (compact.size() == 5 && compact.at(2) == ':') {
        compact.erase(2, 1);
    }
    auto const hours = digitsAt(compact, 0, 2);
    auto const minutes = digitsAt(compact, 2, 2);
    if (compact.size() != 4 || !hours || !minutes) {
        return std::nullopt;
    }
    return *local - (sign * (std::chrono::hours{*hours} + std::chrono::minutes{*minutes}));
}

/// @brief Writes an instant as `YYYY-MM-DDTHH:MM:SS`.
/// @param instant The instant.
/// @return The wall-clock text.
[[nodiscard]] inline std::string formatWallClock(std::chrono::sys_seconds instant) {
    auto const days = std::chrono::floor<std::chrono::days>(instant);
    std::chrono::year_month_day const date{days};
    std::chrono::hh_mm_ss const clock{instant - days};
    return std::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}", static_cast<int>(date.year()),
                       static_cast<unsigned>(date.month()), static_cast<unsigned>(date.day()), clock.hours().count(),
                       clock.minutes().count(), clock.seconds().count());
}

/// @brief Writes an instant as ISO-8601 UTC, `YYYY-MM-DDTHH:MM:SSZ`.
/// @param instant The instant.
/// @return The text.
[[nodiscard]] inline std::string formatUtc(std::chrono::sys_seconds instant) { return formatWallClock(instant) + "Z"; }

/// @brief Whether @p text is a valid calendar date `YYYY-MM-DD`.
/// @param text The text.
/// @return `true` for a well-formed, existing date.
[[nodiscard]] inline bool isIsoDate(std::string_view text) {
    if (text.size() != 10 || text.at(4) != '-' || text.at(7) != '-') {
        return false;
    }
    auto const year = digitsAt(text, 0, 4);
    auto const month = digitsAt(text, 5, 2);
    auto const day = digitsAt(text, 8, 2);
    if (!year || !month || !day) {
        return false;
    }
    return std::chrono::year_month_day{std::chrono::year{*year}, std::chrono::month{static_cast<unsigned>(*month)},
                                       std::chrono::day{static_cast<unsigned>(*day)}}
        .ok();
}

}  // namespace morph::forms::detail::engine

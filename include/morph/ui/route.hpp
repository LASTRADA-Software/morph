// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "../util/rational.hpp"
#include "document.hpp"
#include "value.hpp"

/// @file
/// @brief Routes (spec 6 §20): the string form of a screen instance, `<screen id>?<param>=<value>&…`, written from a
///        screen's declared params and read back against them.
///
/// Specified in `docs/spec/ui/host.md`.

namespace morph::ui {

/// @brief A route read back: the screen and its params.
struct Route {
    /// @brief The screen id.
    std::string screen;
    /// @brief The params, by name; a null optional is absent.
    ValueRecord params;

    /// @brief Equality of screen and params.
    /// @param other The route to compare with.
    /// @return Whether both are equal.
    [[nodiscard]] bool operator==(Route const& other) const = default;
};

namespace detail::route {

/// @brief Whether a byte is an RFC 3986 unreserved character, written as is in a route.
/// @param byte The byte.
/// @return It.
[[nodiscard]] constexpr bool unreserved(char byte) noexcept {
    return (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') ||
           byte == '-' || byte == '.' || byte == '_' || byte == '~';
}

/// @brief Percent-encodes every byte that is not an RFC 3986 unreserved character.
/// @param text The text.
/// @return It, encoded.
[[nodiscard]] inline std::string encode(std::string_view text) {
    static constexpr std::string_view kHex = "0123456789ABCDEF";
    std::string out;
    out.reserve(text.size());
    for (char const byte : text) {
        if (unreserved(byte)) {
            out += byte;
        } else {
            auto const code = static_cast<unsigned char>(byte);
            out += '%';
            out += kHex.at(code >> 4U);
            out += kHex.at(code & 0x0FU);
        }
    }
    return out;
}

/// @brief The value of a hexadecimal digit.
/// @param digit The digit.
/// @return 0 to 15, or -1 for a byte that is not one.
[[nodiscard]] constexpr int hexDigit(char digit) noexcept {
    if (digit >= '0' && digit <= '9') {
        return digit - '0';
    }
    if (digit >= 'A' && digit <= 'F') {
        return digit - 'A' + 10;
    }
    if (digit >= 'a' && digit <= 'f') {
        return digit - 'a' + 10;
    }
    return -1;
}

/// @brief Decodes `%XX` escapes.
/// @param text The text.
/// @return It, decoded; nothing for a truncated or non-hexadecimal escape.
[[nodiscard]] inline std::optional<std::string> decode(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        char const byte = text.at(i);
        if (byte != '%') {
            out += byte;
            continue;
        }
        if (i + 2 >= text.size()) {
            return std::nullopt;
        }
        int const high = hexDigit(text.at(i + 1));
        int const low = hexDigit(text.at(i + 2));
        if (high < 0 || low < 0) {
            return std::nullopt;
        }
        out += static_cast<char>((static_cast<unsigned>(high) << 4U) | static_cast<unsigned>(low));
        i += 2;
    }
    return out;
}

/// @brief Reads a whole decimal integer.
/// @param text The text.
/// @return The integer; nothing for anything else, an empty text and a `+` sign included.
[[nodiscard]] inline std::optional<std::int64_t> integer(std::string_view text) {
    std::int64_t number = 0;
    char const* const last = std::to_address(text.end());
    auto const [end, error] = std::from_chars(std::to_address(text.begin()), last, number);
    if (error != std::errc{} || end != last || text.empty()) {
        return std::nullopt;
    }
    return number;
}

/// @brief The canonical text of a param's value.
/// @param type  The param's declared type.
/// @param value The value.
/// @return The text; nothing for a type with no route form, or a value not of the type.
[[nodiscard]] inline std::optional<std::string> textOf(Type const& type, Value const& value) {
    switch (type.kind) {
        case TypeKind::Int:
            if (std::int64_t const* const number = value.asInt()) {
                return std::to_string(*number);
            }
            return std::nullopt;
        case TypeKind::Bool:
            if (bool const* const flag = value.asBool()) {
                return *flag ? "true" : "false";
            }
            return std::nullopt;
        case TypeKind::String:
            if (std::string const* const text = value.asString()) {
                return encode(*text);
            }
            return std::nullopt;
        case TypeKind::Decimal:
            if (Decimal const* const number = value.asDecimal()) {
                return std::to_string(number->value.numerator) + "/" + std::to_string(number->value.denominator) +
                       "@" + std::to_string(number->value.decimalPlaces.value);
            }
            return std::nullopt;
        case TypeKind::Key:
            if (std::int64_t const* const number = value.asInt()) {
                return "i:" + std::to_string(*number);
            }
            if (std::string const* const text = value.asString()) {
                return "s:" + encode(*text);
            }
            return std::nullopt;
        case TypeKind::Any:
        case TypeKind::Quantity:
        case TypeKind::Timestamp:
        case TypeKind::Record:
        case TypeKind::List:
        default:
            return std::nullopt;
    }
}

/// @brief Reads a param's text.
/// @param type The param's declared type.
/// @param text The text.
/// @return The value; nothing when the text does not parse as the type.
[[nodiscard]] inline std::optional<Value> valueOf(Type const& type, std::string_view text) {
    switch (type.kind) {
        case TypeKind::Int:
            if (auto const number = integer(text)) {
                return Value{*number};
            }
            return std::nullopt;
        case TypeKind::Bool:
            if (text == "true" || text == "false") {
                return Value{text == "true"};
            }
            return std::nullopt;
        case TypeKind::String:
            if (auto decoded = decode(text)) {
                return Value{std::move(*decoded)};
            }
            return std::nullopt;
        case TypeKind::Decimal: {
            auto const slash = text.find('/');
            auto const atSign = text.find('@');
            if (slash == std::string_view::npos || atSign == std::string_view::npos || atSign < slash) {
                return std::nullopt;
            }
            auto const num = integer(text.substr(0, slash));
            auto const den = integer(text.substr(slash + 1, atSign - slash - 1));
            auto const places = integer(text.substr(atSign + 1));
            if (!num || !den || !places || *den <= 0 || *places < 0 ||
                std::cmp_greater(*places, math::kMaxDecimalPlaces)) {
                return std::nullopt;
            }
            return Value{Decimal{math::Rational{math::Numerator{*num}, math::Denominator{*den},
                                                math::DecimalPlaces{static_cast<std::uint32_t>(*places)}}}};
        }
        case TypeKind::Key:
            if (text.starts_with("i:")) {
                if (auto const number = integer(text.substr(2))) {
                    return Value{*number};
                }
                return std::nullopt;
            }
            if (text.starts_with("s:")) {
                if (auto decoded = decode(text.substr(2))) {
                    return Value{std::move(*decoded)};
                }
            }
            return std::nullopt;
        case TypeKind::Any:
        case TypeKind::Quantity:
        case TypeKind::Timestamp:
        case TypeKind::Record:
        case TypeKind::List:
        default:
            return std::nullopt;
    }
}

}  // namespace detail::route

/// @brief Writes the route of a screen instance: its id, then each param the screen declares that @p params gives,
///        in declaration order, as `name=value` joined by `&` after a `?`.
///
/// A param's value is the canonical text of its declared type: an `int` in decimal, a `bool` as `true` or `false`, a
/// `decimal` as `num/den@dp` (the canonical fraction and the places), a `key` as `i:<int64>` or `s:<text>`, and a
/// `string` percent-encoded (every byte but RFC 3986's unreserved characters). A null or absent optional is left
/// out. `quantity`, `timestamp`, list and record params have no route form.
/// @param screen   The screen id.
/// @param declared The screen's declared params (`Document::params`), in order.
/// @param params   The params of this instance.
/// @return The route, or why the instance has none: a param of a type with no route form, a required param that is
///         missing or null, or a value that does not have its declared type.
[[nodiscard]] inline std::expected<std::string, std::string> routeOf(
    std::string_view screen, std::vector<std::pair<std::string, Type>> const& declared, ValueRecord const& params) {
    std::string route{screen};
    char separator = '?';
    for (auto const& [name, type] : declared) {
        auto const given = params.find(name);
        if (given == params.end() || given->second.isNull()) {
            if (!type.optional) {
                return std::unexpected("the required param " + name + " is missing");
            }
            continue;
        }
        auto text = detail::route::textOf(type, given->second);
        if (!text) {
            return std::unexpected("the param " + name + " has no route form for its value");
        }
        route += separator;
        route += detail::route::encode(name);
        route += '=';
        route += *text;
        separator = '&';
    }
    return route;
}

/// @brief Writes the route of a mounted document's instance: `routeOf` with the document's id and declared params.
/// @param document The document.
/// @param params   The params of this instance.
/// @return The route, or why it has none.
[[nodiscard]] inline std::expected<std::string, std::string> routeOf(Document const& document,
                                                                     ValueRecord const& params) {
    return routeOf(document.id, document.params, params);
}

/// @brief Reads a route back against the screen it names.
///
/// Refused, with the route and the reason: a screen @p lookup does not know, a param the screen does not declare or
/// gives twice, a value that does not parse as its declared type, and a required param that is missing.
/// @param route  The route text.
/// @param lookup The document of a screen id, or null for an unknown one.
/// @return The screen and its params, or why the route is refused.
[[nodiscard]] inline std::expected<Route, std::string> parseRoute(
    std::string_view route, std::function<std::shared_ptr<Document const>(std::string_view)> const& lookup) {
    auto const refuse = [route](std::string const& reason) {
        return std::unexpected("route " + std::string{route} + ": " + reason);
    };
    auto const question = route.find('?');
    Route parsed{.screen = std::string{route.substr(0, question)}, .params = {}};
    std::shared_ptr<Document const> const document = lookup ? lookup(parsed.screen) : nullptr;
    if (!document) {
        return refuse("unknown screen " + parsed.screen);
    }
    std::string_view rest = question == std::string_view::npos ? std::string_view{} : route.substr(question + 1);
    while (!rest.empty()) {
        auto const amp = rest.find('&');
        std::string_view const pair = rest.substr(0, amp);
        rest = amp == std::string_view::npos ? std::string_view{} : rest.substr(amp + 1);
        auto const equals = pair.find('=');
        if (equals == std::string_view::npos) {
            return refuse("a param without a value");
        }
        auto name = detail::route::decode(pair.substr(0, equals));
        if (!name) {
            return refuse("a param name that does not decode");
        }
        auto const declared = std::ranges::find(document->params, *name, &std::pair<std::string, Type>::first);
        if (declared == document->params.end()) {
            return refuse("the screen declares no param " + *name);
        }
        if (parsed.params.contains(*name)) {
            return refuse("the param " + *name + " is given twice");
        }
        auto value = detail::route::valueOf(declared->second, pair.substr(equals + 1));
        if (!value) {
            return refuse("the param " + *name + " does not parse as its type");
        }
        parsed.params.emplace(std::move(*name), std::move(*value));
    }
    for (auto const& [name, type] : document->params) {
        if (!type.optional && !parsed.params.contains(name)) {
            return refuse("the required param " + name + " is missing");
        }
    }
    return parsed;
}

}  // namespace morph::ui

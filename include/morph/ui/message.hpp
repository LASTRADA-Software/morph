// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../render/locale_format.hpp"
#include "../util/rational.hpp"
#include "value.hpp"

/// @file
/// @brief The MessageFormat 2 subset `ui/1` accepts for `t(key, fallback, args)`, and the CLDR plural rules its
///        `.match` on a `:number` selects by.
///
/// Specified in `docs/spec/ui/expressions.md`, "Messages".

namespace morph::ui {

/// @brief A CLDR plural category.
enum class PluralCategory : std::uint8_t {
    Zero,   ///< `zero`
    One,    ///< `one`
    Two,    ///< `two`
    Few,    ///< `few`
    Many,   ///< `many`
    Other,  ///< `other`
};

/// @brief The keyword of a plural category, as a `.match` key spells it.
/// @param category The category.
/// @return `zero`, `one`, `two`, `few`, `many` or `other`.
[[nodiscard]] constexpr std::string_view categoryName(PluralCategory category) {
    constexpr std::array<std::string_view, 6> kNames = {"zero", "one", "two", "few", "many", "other"};
    return kNames.at(static_cast<std::size_t>(category));
}

namespace detail {

/// @brief The decimal text of an exact value at a number of places: rounded half to even, then written with exactly
///        that many fraction digits, through integer arithmetic only.
/// @param value  The exact value.
/// @param places The number of fraction digits, at most `math::kMaxDecimalPlaces`.
/// @return Canonical text `-?[0-9]+(\.[0-9]+)?`; no sign on a value that rounds to zero.
[[nodiscard]] inline std::string decimalText(math::Rational const& value, std::uint32_t places) {
    math::Rational const rounded =
        math::roundToDecimalPlaces(value, math::DecimalPlaces{places}, math::RoundingMode::HalfEven);
    std::uint64_t const magnitude = math::detail::absU64(rounded.numerator);
    auto const denominator = static_cast<std::uint64_t>(rounded.denominator);
    std::string out = rounded.numerator < 0 ? "-" : "";
    out += std::to_string(magnitude / denominator);
    // The rounded value is a multiple of 10^-places, so its denominator divides 10^places <= 10^18: the remainder
    // times ten stays below 10^19, inside uint64, and the expansion ends within `places` digits.
    std::uint64_t remainder = magnitude % denominator;
    if (places > 0) {
        out += '.';
        for (std::uint32_t digit = 0; digit < places; ++digit) {
            remainder *= 10U;
            out += static_cast<char>('0' + static_cast<char>(remainder / denominator));
            remainder %= denominator;
        }
    }
    return out;
}

/// @brief The places a number is shown with: an `int` has none, a decimal or quantity its declared places.
/// @param value The number.
/// @return The places, or `std::nullopt` when @p value is not a number.
[[nodiscard]] inline std::optional<std::pair<math::Rational, std::uint32_t>> numberOf(Value const& value) {
    if (std::int64_t const* const whole = value.asInt()) {
        return std::pair{math::Rational{*whole, math::DecimalPlaces{0}}, 0U};
    }
    if (Decimal const* const decimal = value.asDecimal()) {
        return std::pair{decimal->value, decimal->value.decimalPlaces.value};
    }
    if (Quantity const* const quantity = value.asQuantity()) {
        return std::pair{quantity->value, quantity->value.decimalPlaces.value};
    }
    return std::nullopt;
}

}  // namespace detail

/// @brief Formats a number for display in a locale: at its own places, rounded half to even.
/// @param value   An `int`, `decimal` or `quantity` (its magnitude).
/// @param numeric The locale's separators, sign and digits.
/// @return The display text, or `std::nullopt` when @p value is not a number.
[[nodiscard]] inline std::optional<std::string> formatNumber(Value const& value,
                                                             render::NumericLocale const& numeric) {
    auto const number = detail::numberOf(value);
    if (!number) {
        return std::nullopt;
    }
    return render::formatCanonicalNumber(detail::decimalText(number->first, number->second), numeric);
}

/// @brief The CLDR plural category of a number in a locale.
///
/// The operands are CLDR's: `n` the absolute value, `i` its integer digits, `v` the count of visible fraction
/// digits (the decimal's places; an `int` has none). Rules are carried for `en`, `de`, `nl`, `sv`, `it`, `es`,
/// `pt`, `fr`, `pl`, `ru`, `uk`, `cs` and `ar`; `ja`, `zh`, `ko` and every other language select only `other`. The
/// locale's language subtag decides, case-insensitively, so `pl-PL` uses the Polish rules.
/// @param locale A BCP 47 locale, such as `pl-PL`.
/// @param value  The exact number.
/// @param places The visible fraction digits.
/// @return The category.
[[nodiscard]] inline PluralCategory pluralCategory(std::string_view locale, math::Rational const& value,
                                                   std::uint32_t places) {
    std::string language;
    for (char const chr : locale) {
        if (chr == '-' || chr == '_') {
            break;
        }
        language += static_cast<char>(std::tolower(static_cast<unsigned char>(chr)));
    }
    math::Rational const shown =
        math::roundToDecimalPlaces(value, math::DecimalPlaces{places}, math::RoundingMode::HalfEven);
    std::uint64_t const integer =
        math::detail::absU64(shown.numerator) / static_cast<std::uint64_t>(shown.denominator);
    bool const whole = shown.denominator == 1;
    bool const visibleFraction = places != 0;  // CLDR `v != 0`
    auto const in = [](std::uint64_t operand, std::uint64_t low, std::uint64_t high) {
        return operand >= low && operand <= high;
    };
    std::uint64_t const mod10 = integer % 10U;
    std::uint64_t const mod100 = integer % 100U;
    bool const million = !visibleFraction && integer != 0 && integer % 1000000U == 0;

    if (language == "en" || language == "de" || language == "nl" || language == "sv" || language == "it") {
        if (integer == 1 && !visibleFraction) {
            return PluralCategory::One;
        }
        return language == "it" && million ? PluralCategory::Many : PluralCategory::Other;
    }
    if (language == "es") {
        if (whole && integer == 1) {
            return PluralCategory::One;
        }
        return million ? PluralCategory::Many : PluralCategory::Other;
    }
    if (language == "fr" || language == "pt") {
        if (integer <= 1) {
            return PluralCategory::One;
        }
        return million ? PluralCategory::Many : PluralCategory::Other;
    }
    if (language == "pl") {
        if (integer == 1 && !visibleFraction) {
            return PluralCategory::One;
        }
        if (visibleFraction) {
            return PluralCategory::Other;
        }
        if (in(mod10, 2, 4) && !in(mod100, 12, 14)) {
            return PluralCategory::Few;
        }
        return PluralCategory::Many;
    }
    if (language == "ru" || language == "uk") {
        if (visibleFraction) {
            return PluralCategory::Other;
        }
        if (mod10 == 1 && mod100 != 11) {
            return PluralCategory::One;
        }
        if (in(mod10, 2, 4) && !in(mod100, 12, 14)) {
            return PluralCategory::Few;
        }
        return PluralCategory::Many;
    }
    if (language == "cs") {
        if (visibleFraction) {
            return PluralCategory::Many;
        }
        if (integer == 1) {
            return PluralCategory::One;
        }
        return in(integer, 2, 4) ? PluralCategory::Few : PluralCategory::Other;
    }
    if (language == "ar") {
        if (!whole) {
            return PluralCategory::Other;
        }
        if (integer <= 2) {
            return integer == 0 ? PluralCategory::Zero : (integer == 1 ? PluralCategory::One : PluralCategory::Two);
        }
        if (in(mod100, 3, 10)) {
            return PluralCategory::Few;
        }
        return in(mod100, 11, 99) ? PluralCategory::Many : PluralCategory::Other;
    }
    return PluralCategory::Other;
}

/// @brief The formatting function a placeholder or a declaration names.
enum class MessageFunction : std::uint8_t {
    None,    ///< No annotation: a string as it is, a number as `:number` formats it.
    Number,  ///< `:number`: locale number formatting, and plural selection.
    String,  ///< `:string`: the text, and selection by exact match.
};

/// @brief One part of a pattern: literal text, or a placeholder of a variable.
struct MessagePart {
    /// @brief The literal text; empty for a placeholder.
    std::string text;
    /// @brief The variable a placeholder reads, without `$`; empty for text.
    std::string variable;
    /// @brief The placeholder's annotation.
    MessageFunction function = MessageFunction::None;
};

/// @brief One variant of a `.match`: a key per selector (`*` is the catch-all) and its pattern.
struct MessageVariant {
    /// @brief The keys, one per selector.
    std::vector<std::string> keys;
    /// @brief The pattern shown when the variant is selected.
    std::vector<MessagePart> pattern;
};

/// @brief A parsed message in the `ui/1` subset of MessageFormat 2.
struct MessagePattern {
    /// @brief The `.input` declarations: each variable with its function.
    std::vector<std::pair<std::string, MessageFunction>> inputs;
    /// @brief The `.match` selectors, as variable names; empty for a message without `.match`.
    std::vector<std::string> selectors;
    /// @brief The variants of a `.match`.
    std::vector<MessageVariant> variants;
    /// @brief The pattern of a message without `.match`.
    std::vector<MessagePart> pattern;
};

namespace detail {

/// @brief A recursive-descent reader over one message's text.
class MessageReader {
public:
    /// @brief Reads @p text.
    /// @param text The message source.
    explicit MessageReader(std::string_view text) : _text{text} {}

    /// @brief Parses the whole message.
    /// @return The pattern, or the reason it is outside the subset.
    [[nodiscard]] std::expected<MessagePattern, std::string> parse() {
        MessagePattern message;
        std::size_t const start = _pos;
        skipSpace();
        if (!atEnd() && (peek() == '.' || _text.substr(_pos).starts_with("{{"))) {
            return parseComplex(message);
        }
        _pos = start;
        auto pattern = parsePattern(false);
        if (!pattern) {
            return std::unexpected(pattern.error());
        }
        message.pattern = std::move(*pattern);
        return message;
    }

private:
    [[nodiscard]] bool atEnd() const noexcept { return _pos >= _text.size(); }
    [[nodiscard]] char peek() const noexcept { return _text.at(_pos); }

    void skipSpace() noexcept {
        while (!atEnd() && (peek() == ' ' || peek() == '\t' || peek() == '\n' || peek() == '\r')) {
            ++_pos;
        }
    }

    [[nodiscard]] static bool nameChar(char chr, bool first) noexcept {
        bool const letter = (chr >= 'a' && chr <= 'z') || (chr >= 'A' && chr <= 'Z') || chr == '_';
        if (first) {
            return letter;
        }
        return letter || (chr >= '0' && chr <= '9') || chr == '-' || chr == '.';
    }

    [[nodiscard]] std::string readName() {
        std::size_t const start = _pos;
        while (!atEnd() && nameChar(peek(), _pos == start)) {
            ++_pos;
        }
        return std::string{_text.substr(start, _pos - start)};
    }

    /// Reads `$name`; the caller has seen the `$`.
    [[nodiscard]] std::expected<std::string, std::string> readVariable() {
        ++_pos;
        std::string name = readName();
        if (name.empty()) {
            return std::unexpected(std::string{"a variable needs a name after `$`"});
        }
        return name;
    }

    /// Reads `{ $var [:number|:string] }` after the opening brace has been consumed.
    [[nodiscard]] std::expected<std::pair<std::string, MessageFunction>, std::string> readExpressionBody() {
        skipSpace();
        if (atEnd()) {
            return std::unexpected(std::string{"a placeholder is not closed"});
        }
        if (peek() != '$') {
            return std::unexpected(std::string{"only variable placeholders `{$name}` are in the ui/1 subset"});
        }
        auto variable = readVariable();
        if (!variable) {
            return std::unexpected(variable.error());
        }
        skipSpace();
        MessageFunction function = MessageFunction::None;
        if (!atEnd() && peek() == ':') {
            ++_pos;
            std::string const name = readName();
            if (name == "number") {
                function = MessageFunction::Number;
            } else if (name == "string") {
                function = MessageFunction::String;
            } else {
                return std::unexpected("the function `:" + name + "` is not in the ui/1 subset");
            }
            skipSpace();
        }
        if (atEnd() || peek() != '}') {
            return std::unexpected(std::string{"options and attributes are not in the ui/1 subset"});
        }
        ++_pos;
        return std::pair{std::move(*variable), function};
    }

    /// Reads a pattern up to the end of the text (simple) or up to `}}` (quoted).
    [[nodiscard]] std::expected<std::vector<MessagePart>, std::string> parsePattern(bool quoted) {
        std::vector<MessagePart> parts;
        std::string text;
        auto flush = [&] {
            if (!text.empty()) {
                parts.push_back(MessagePart{.text = std::move(text), .variable = {}, .function = {}});
                text.clear();
            }
        };
        while (!atEnd()) {
            char const chr = peek();
            if (quoted && _text.substr(_pos).starts_with("}}")) {
                _pos += 2;
                flush();
                return parts;
            }
            if (chr == '\\') {
                ++_pos;
                if (atEnd() || (peek() != '\\' && peek() != '{' && peek() != '}' && peek() != '|')) {
                    return std::unexpected(std::string{"`\\` escapes only `\\`, `{`, `}` and `|`"});
                }
                text += peek();
                ++_pos;
                continue;
            }
            if (chr == '{') {
                ++_pos;
                auto body = readExpressionBody();
                if (!body) {
                    return std::unexpected(body.error());
                }
                flush();
                parts.push_back(MessagePart{.text = {}, .variable = std::move(body->first), .function = body->second});
                continue;
            }
            if (chr == '}') {
                return std::unexpected(std::string{"an unescaped `}` in text"});
            }
            text += chr;
            ++_pos;
        }
        if (quoted) {
            return std::unexpected(std::string{"a quoted pattern is not closed with `}}`"});
        }
        flush();
        return parts;
    }

    [[nodiscard]] std::expected<std::string, std::string> readKey() {
        if (peek() == '*') {
            ++_pos;
            return std::string{"*"};
        }
        if (peek() == '|') {
            ++_pos;
            std::string key;
            while (!atEnd() && peek() != '|') {
                if (peek() == '\\') {
                    ++_pos;
                    if (atEnd()) {
                        break;
                    }
                }
                key += peek();
                ++_pos;
            }
            if (atEnd()) {
                return std::unexpected(std::string{"a quoted key is not closed with `|`"});
            }
            ++_pos;
            return key;
        }
        std::size_t const start = _pos;
        if (peek() == '=') {
            ++_pos;
        }
        while (!atEnd() && nameChar(peek(), false)) {
            ++_pos;
        }
        if (_pos == start) {
            return std::unexpected(std::string{"a variant key is expected"});
        }
        return std::string{_text.substr(start, _pos - start)};
    }

    [[nodiscard]] std::expected<MessagePattern, std::string> parseComplex(MessagePattern& message) {
        for (;;) {
            skipSpace();
            if (_text.substr(_pos).starts_with(".input")) {
                _pos += 6;
                skipSpace();
                if (atEnd() || peek() != '{') {
                    return std::unexpected(std::string{"`.input` takes a `{$name :function}`"});
                }
                ++_pos;
                auto body = readExpressionBody();
                if (!body) {
                    return std::unexpected(body.error());
                }
                message.inputs.push_back(std::move(*body));
                continue;
            }
            if (_text.substr(_pos).starts_with(".match")) {
                _pos += 6;
                return parseMatch(message);
            }
            if (_text.substr(_pos).starts_with("{{")) {
                _pos += 2;
                auto pattern = parsePattern(true);
                if (!pattern) {
                    return std::unexpected(pattern.error());
                }
                message.pattern = std::move(*pattern);
                skipSpace();
                if (!atEnd()) {
                    return std::unexpected(std::string{"text after the quoted pattern"});
                }
                return message;
            }
            return std::unexpected(std::string{"only `.input`, `.match` and a quoted pattern are in the ui/1 subset"});
        }
    }

    [[nodiscard]] std::expected<MessagePattern, std::string> parseMatch(MessagePattern& message) {
        for (;;) {
            skipSpace();
            if (atEnd() || peek() != '$') {
                break;
            }
            auto selector = readVariable();
            if (!selector) {
                return std::unexpected(selector.error());
            }
            bool annotated = false;
            for (auto const& [name, function] : message.inputs) {
                annotated = annotated || (name == *selector && function != MessageFunction::None);
            }
            if (!annotated) {
                return std::unexpected("the selector `$" + *selector +
                                       "` needs an `.input` declaration with `:number` or `:string`");
            }
            message.selectors.push_back(std::move(*selector));
        }
        if (message.selectors.empty()) {
            return std::unexpected(std::string{"`.match` needs at least one selector"});
        }
        bool catchAll = false;
        for (;;) {
            skipSpace();
            if (atEnd()) {
                break;
            }
            MessageVariant variant;
            while (!atEnd() && !_text.substr(_pos).starts_with("{{")) {
                auto key = readKey();
                if (!key) {
                    return std::unexpected(key.error());
                }
                variant.keys.push_back(std::move(*key));
                skipSpace();
            }
            if (atEnd()) {
                return std::unexpected(std::string{"a variant needs a quoted pattern `{{...}}`"});
            }
            if (variant.keys.size() != message.selectors.size()) {
                return std::unexpected(std::string{"a variant has one key per selector"});
            }
            _pos += 2;
            auto pattern = parsePattern(true);
            if (!pattern) {
                return std::unexpected(pattern.error());
            }
            variant.pattern = std::move(*pattern);
            bool allStars = true;
            for (std::string const& key : variant.keys) {
                allStars = allStars && key == "*";
            }
            catchAll = catchAll || allStars;
            message.variants.push_back(std::move(variant));
        }
        if (!catchAll) {
            return std::unexpected(std::string{"`.match` needs a catch-all variant `*`"});
        }
        return message;
    }

    std::string_view _text;
    std::size_t _pos = 0;
};

}  // namespace detail

/// @brief Parses a message in the `ui/1` subset of MessageFormat 2.
///
/// The subset is: text with `\` escapes; placeholders `{$name}`, `{$name :number}` and `{$name :string}`; `.input`
/// declarations of the same form; and `.match` over declared `:number` or `:string` variables, with variant keys
/// that are plural categories, exact numbers (`0`, `=0`), exact strings, or `*`, and a required catch-all.
/// Literal placeholders, options, markup and `.local` are outside it.
/// @param text The message source.
/// @return The parsed message, or the reason it is refused.
[[nodiscard]] inline std::expected<MessagePattern, std::string> parseMessage(std::string_view text) {
    return detail::MessageReader{text}.parse();
}

/// @brief The locale a message is formatted for.
struct MessageLocale {
    /// @brief The BCP 47 locale, which selects the plural rules.
    std::string locale = "en";
    /// @brief The locale's number formatting.
    render::NumericLocale numeric{};
};

/// @brief A formatted message, and the variables it could not format.
struct FormattedMessage {
    /// @brief The text; a variable that could not be formatted shows as `{$name}`.
    std::string text;
    /// @brief One line per variable that was missing, `null`, or of the wrong kind for its function.
    std::vector<std::string> errors;
};

namespace detail {

/// @brief Reads an exact-number variant key: `-?[0-9]+(\.[0-9]+)?`, at most 18 digits.
/// @param key The key, without a leading `=`.
/// @return The exact value, or `std::nullopt` when the key is not such a number.
[[nodiscard]] inline std::optional<math::Rational> parseExactKey(std::string_view key) {
    bool const negative = key.starts_with('-');
    if (negative) {
        key.remove_prefix(1);
    }
    std::int64_t numerator = 0;
    std::int64_t denominator = 1;
    std::uint32_t digits = 0;
    bool fraction = false;
    for (char const chr : key) {
        if (chr == '.' && !fraction && digits > 0) {
            fraction = true;
            continue;
        }
        if (chr < '0' || chr > '9' || ++digits > math::kMaxDecimalPlaces) {
            return std::nullopt;
        }
        numerator = (numerator * 10) + (chr - '0');
        if (fraction) {
            denominator *= 10;
        }
    }
    if (digits == 0 || key.ends_with('.')) {
        return std::nullopt;
    }
    return math::Rational{math::Numerator{negative ? -numerator : numerator}, math::Denominator{denominator},
                          math::DecimalPlaces{0}};
}

/// @brief Whether a variant key matches a selector's value.
/// @param key      The key.
/// @param value    The selector's value.
/// @param function The selector's function.
/// @param locale   The locale for plural rules.
/// @return 0 for an exact match, 1 for a plural-category match, 2 for `*`, `std::nullopt` for no match.
[[nodiscard]] inline std::optional<int> matchKey(std::string_view key, Value const& value, MessageFunction function,
                                                 MessageLocale const& locale) {
    if (key == "*") {
        return 2;
    }
    if (function == MessageFunction::String) {
        std::string const* const text = value.asString();
        if (text != nullptr && *text == key) {
            return 0;
        }
        return std::nullopt;
    }
    auto const number = numberOf(value);
    if (!number) {
        return std::nullopt;
    }
    std::string_view exact = key;
    if (exact.starts_with('=')) {
        exact.remove_prefix(1);
    }
    if (auto const wanted = parseExactKey(exact)) {
        // An exact key compares by value with the number as shown, so `1` matches 1, 1.0 and 1.00.
        math::Rational const shown = math::roundToDecimalPlaces(number->first, math::DecimalPlaces{number->second},
                                                                math::RoundingMode::HalfEven);
        if (shown == *wanted) {
            return 0;
        }
        return std::nullopt;
    }
    if (categoryName(pluralCategory(locale.locale, number->first, number->second)) == key) {
        return 1;
    }
    return std::nullopt;
}

/// @brief Formats one pattern.
/// @param pattern   The parts.
/// @param args      The named arguments.
/// @param functions The `.input` declarations, which annotate a bare placeholder of their variable.
/// @param locale    The locale.
/// @param out       The message being built.
inline void formatPattern(std::vector<MessagePart> const& pattern, ValueRecord const& args,
                          std::vector<std::pair<std::string, MessageFunction>> const& functions,
                          MessageLocale const& locale, FormattedMessage& out) {
    for (MessagePart const& part : pattern) {
        if (part.variable.empty()) {
            out.text += part.text;
            continue;
        }
        MessageFunction function = part.function;
        for (auto const& [name, declared] : functions) {
            if (name == part.variable && function == MessageFunction::None) {
                function = declared;
            }
        }
        auto const found = args.find(part.variable);
        Value const missing{};
        Value const& value = found == args.end() ? missing : found->second;
        if (std::string const* const text = value.asString(); text != nullptr && function != MessageFunction::Number) {
            out.text += *text;
            continue;
        }
        if (auto number = formatNumber(value, locale.numeric)) {
            out.text += *number;
            continue;
        }
        out.text += "{$" + part.variable + "}";
        out.errors.push_back(found == args.end() || value.isNull()
                                 ? "the argument `" + part.variable + "` is missing"
                                 : "the argument `" + part.variable + "` is not a " +
                                       (function == MessageFunction::Number ? "number" : "string or number"));
    }
}

}  // namespace detail

/// @brief Formats a parsed message with named arguments.
///
/// A `.match` selects the variant whose keys match best, selector by selector: an exact key before a plural
/// category before `*`.
/// @param message The parsed message.
/// @param args    The named arguments.
/// @param locale  The locale.
/// @return The text and the errors of the variables that could not be formatted.
[[nodiscard]] inline FormattedMessage formatMessage(MessagePattern const& message, ValueRecord const& args,
                                                    MessageLocale const& locale) {
    FormattedMessage out;
    if (message.selectors.empty()) {
        detail::formatPattern(message.pattern, args, message.inputs, locale, out);
        return out;
    }
    MessageVariant const* best = nullptr;
    std::vector<int> bestScore;
    for (MessageVariant const& variant : message.variants) {
        std::vector<int> score;
        bool matches = true;
        for (std::size_t index = 0; index < message.selectors.size() && matches; ++index) {
            std::string const& selector = message.selectors.at(index);
            MessageFunction function = MessageFunction::None;
            for (auto const& [name, declared] : message.inputs) {
                if (name == selector) {
                    function = declared;
                }
            }
            auto const found = args.find(selector);
            Value const missing{};
            auto const rank = detail::matchKey(variant.keys.at(index), found == args.end() ? missing : found->second,
                                               function, locale);
            matches = rank.has_value();
            score.push_back(rank.value_or(3));
        }
        if (matches && (best == nullptr || score < bestScore)) {
            best = &variant;
            bestScore = std::move(score);
        }
    }
    // parseMessage requires a catch-all variant, so one always matches.
    if (best != nullptr) {
        detail::formatPattern(best->pattern, args, message.inputs, locale, out);
    }
    return out;
}

}  // namespace morph::ui

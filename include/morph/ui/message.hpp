// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <algorithm>
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

/// @file
/// @brief The subset of Unicode MessageFormat 2 a UI document's text is written in, and the CLDR plural rules its
///        `.match` selects by.
///
/// Specified in `docs/spec/ui/expressions.md` ("Messages").

namespace morph::ui {

/// @brief The CLDR plural categories.
enum class PluralCategory : std::uint8_t {
    Zero,   ///< `zero`
    One,    ///< `one`
    Two,    ///< `two`
    Few,    ///< `few`
    Many,   ///< `many`
    Other,  ///< `other`
};

/// @brief A plural category's CLDR keyword.
/// @param category The category.
/// @return Its keyword, for example `"few"`.
[[nodiscard]] constexpr std::string_view pluralKeyword(PluralCategory category) noexcept {
    switch (category) {
        case PluralCategory::Zero:
            return "zero";
        case PluralCategory::One:
            return "one";
        case PluralCategory::Two:
            return "two";
        case PluralCategory::Few:
            return "few";
        case PluralCategory::Many:
            return "many";
        case PluralCategory::Other:
        default:
            return "other";
    }
}

/// @brief The operands of a CLDR plural rule, read from a number as it is displayed.
struct PluralOperands {
    /// @brief `i`: the integer digits, as an integer.
    std::uint64_t integer = 0;
    /// @brief `v`: how many fraction digits are shown, trailing zeros included.
    std::uint32_t visibleFractionDigits = 0;
    /// @brief `f`: the shown fraction digits, as an integer.
    std::uint64_t fraction = 0;
};

namespace detail {

// 10^places, for places up to kMaxDecimalPlaces.
[[nodiscard]] constexpr std::uint64_t powerOfTen(std::uint32_t places) noexcept {
    std::uint64_t out = 1;
    for (std::uint32_t digit = 0; digit < places; ++digit) {
        out *= 10U;
    }
    return out;
}

}  // namespace detail

/// @brief A decimal as canonical text at @p places, rounded half to even: `-?[0-9]+(\.[0-9]+)?`.
/// @param value The decimal.
/// @param places The places shown.
/// @return The text, for example `"-1.50"` for -3/2 at 2 places.
[[nodiscard]] inline std::string decimalText(math::Rational const& value, std::uint32_t places) {
    places = std::min(places, math::kMaxDecimalPlaces);
    math::Rational const rounded =
        math::roundToDecimalPlaces(value, math::DecimalPlaces{places}, math::RoundingMode::HalfEven);
    bool const negative = rounded.numerator < 0;
    // Magnitudes in uint64: |INT64_MIN| does not fit int64, and a remainder times ten stays below 10^19.
    std::uint64_t const numerator =
        negative ? 0U - static_cast<std::uint64_t>(rounded.numerator) : static_cast<std::uint64_t>(rounded.numerator);
    auto const denominator = static_cast<std::uint64_t>(rounded.denominator);
    std::string out = (negative ? "-" : "") + std::to_string(numerator / denominator);
    if (places == 0) {
        return out;
    }
    out += '.';
    std::uint64_t remainder = numerator % denominator;
    for (std::uint32_t digit = 0; digit < places; ++digit) {
        remainder *= 10U;
        out += static_cast<char>('0' + (remainder / denominator));
        remainder %= denominator;
    }
    return out;
}

/// @brief The plural operands of canonical number text.
/// @param canonical Text as `decimalText` writes it.
/// @return Its operands; the sign is ignored, as CLDR's rules ignore it.
[[nodiscard]] inline PluralOperands pluralOperands(std::string_view canonical) {
    if (canonical.starts_with('-')) {
        canonical.remove_prefix(1);
    }
    std::size_t const dot = canonical.find('.');
    std::string_view const whole = canonical.substr(0, dot);
    std::string_view const fraction = dot == std::string_view::npos ? std::string_view{} : canonical.substr(dot + 1);
    PluralOperands out;
    // Kept modulo 10^18: the rules read `i` modulo 10^6 at most, and a remainder is all that survives the bound.
    constexpr std::uint64_t kBound = 1'000'000'000'000'000'000ULL;
    for (char const digit : whole) {
        out.integer = (out.integer * 10U + static_cast<std::uint64_t>(digit - '0')) % kBound;
    }
    out.visibleFractionDigits = static_cast<std::uint32_t>(fraction.size());
    for (char const digit : fraction.substr(0, 18)) {
        out.fraction = out.fraction * 10U + static_cast<std::uint64_t>(digit - '0');
    }
    return out;
}

/// @brief The CLDR cardinal plural category of a number in a locale.
///
/// Rules are compiled in for `en`, `de`, `fr`, `pl` and `ar`, by the locale's language subtag; any other language
/// reads `other` for every number, as CLDR's root locale does, so a catalogue's exact-value keys and its `*`
/// variant still apply.
/// @param bcp47Locale The locale, for example `"pl-PL"`.
/// @param number The operands of the number as displayed.
/// @return Its category.
[[nodiscard]] inline PluralCategory pluralCategory(std::string_view bcp47Locale, PluralOperands const& number) {
    std::string_view const language = bcp47Locale.substr(0, bcp47Locale.find_first_of("-_"));
    std::uint64_t const i = number.integer;  // NOLINT(readability-identifier-length): CLDR's operand names
    bool const whole = number.visibleFractionDigits == 0;
    if (language == "en" || language == "de") {
        return i == 1 && whole ? PluralCategory::One : PluralCategory::Other;
    }
    if (language == "fr") {
        if (i == 0 || i == 1) {
            return PluralCategory::One;
        }
        return i != 0 && i % 1'000'000U == 0 && whole ? PluralCategory::Many : PluralCategory::Other;
    }
    if (language == "pl") {
        if (!whole) {
            return PluralCategory::Other;
        }
        if (i == 1) {
            return PluralCategory::One;
        }
        std::uint64_t const lastDigit = i % 10U;
        std::uint64_t const lastTwo = i % 100U;
        if (lastDigit >= 2 && lastDigit <= 4 && (lastTwo < 12 || lastTwo > 14)) {
            return PluralCategory::Few;
        }
        return PluralCategory::Many;
    }
    if (language == "ar") {
        // `n` is the absolute value: a shown non-zero fraction makes it no integer, and every rule asks for one.
        if (number.fraction != 0) {
            return PluralCategory::Other;
        }
        std::uint64_t const lastTwo = i % 100U;
        if (i == 0) {
            return PluralCategory::Zero;
        }
        if (i == 1) {
            return PluralCategory::One;
        }
        if (i == 2) {
            return PluralCategory::Two;
        }
        if (lastTwo >= 3 && lastTwo <= 10) {
            return PluralCategory::Few;
        }
        return lastTwo >= 11 ? PluralCategory::Many : PluralCategory::Other;
    }
    return PluralCategory::Other;
}

/// @brief A message in the MessageFormat 2 subset, parsed once.
///
/// The subset: text with placeholders `{$name}`, `{$name :number}` and `{$name :string}`; `\{`, `\}`, `\|` and
/// `\\` escapes; and a complex message of `.input {$name :number}` or `.input {$name :string}` declarations,
/// `.match` on declared names, and variants keyed by a plural category, an exact value (`0` or `=0`), a string,
/// or `*`, each with a quoted pattern `{{...}}`.
class Message {
public:
    /// @brief How a placeholder or a selector reads its argument.
    enum class Function : std::uint8_t {
        None,    ///< As the argument is: a number as `:number`, anything else as `:string`.
        Number,  ///< `:number`: locale digits; selects by plural category, then exact value.
        String,  ///< `:string`: the text; selects by exact text.
    };

    /// @brief One piece of a pattern: text, or a placeholder for an argument.
    struct Part {
        /// @brief The literal text; empty for a placeholder.
        std::string text;
        /// @brief The argument's name, without `$`; empty for text.
        std::string variable;
        /// @brief How the placeholder formats its argument.
        Function function = Function::None;
    };

    /// @brief A pattern: its parts, in order.
    using Pattern = std::vector<Part>;

    /// @brief One variant of a `.match`: one key per selector, and its pattern.
    struct Variant {
        /// @brief The keys, `*` for the catch-all.
        std::vector<std::string> keys;
        /// @brief The text it produces.
        Pattern pattern;
    };

    /// @brief The `.input` declarations: an argument's name and its function.
    std::vector<std::pair<std::string, Function>> inputs;
    /// @brief The `.match` selectors' names; empty for a simple message.
    std::vector<std::string> selectors;
    /// @brief The `.match` variants, in the order written.
    std::vector<Variant> variants;
    /// @brief A simple message's pattern.
    Pattern pattern;
};

namespace detail {

struct MessageReader {
    std::string_view text;
    std::size_t at = 0;

    [[nodiscard]] bool done() const noexcept { return at >= text.size(); }
    [[nodiscard]] char peek() const noexcept { return done() ? '\0' : text.at(at); }

    void skipSpace() {
        while (!done() && (peek() == ' ' || peek() == '\t' || peek() == '\n' || peek() == '\r')) {
            ++at;
        }
    }

    [[nodiscard]] bool take(std::string_view token) {
        if (text.substr(at).starts_with(token)) {
            at += token.size();
            return true;
        }
        return false;
    }

    [[nodiscard]] std::string name() {
        std::size_t const start = at;
        while (!done() && (std::isalnum(static_cast<unsigned char>(peek())) != 0 || peek() == '_' || peek() == '-' ||
                           peek() == '.' || static_cast<unsigned char>(peek()) >= 0x80U)) {
            ++at;
        }
        return std::string{text.substr(start, at - start)};
    }

    [[nodiscard]] std::unexpected<std::string> refuse(std::string reason) const {
        return std::unexpected(std::move(reason) + " at offset " + std::to_string(at));
    }

    // `{$name}`, `{$name :number}` or `{$name :string}`, after its `{`.
    [[nodiscard]] std::expected<Message::Part, std::string> placeholder() {
        skipSpace();
        if (!take("$")) {
            return refuse("a placeholder names an argument, `{$name}`");
        }
        Message::Part part{.text = {}, .variable = name(), .function = Message::Function::None};
        if (part.variable.empty()) {
            return refuse("a placeholder names an argument, `{$name}`");
        }
        skipSpace();
        if (take(":number")) {
            part.function = Message::Function::Number;
        } else if (take(":string")) {
            part.function = Message::Function::String;
        } else if (peek() == ':') {
            return refuse("only `:number` and `:string` are supported");
        }
        skipSpace();
        if (!take("}")) {
            return refuse("a placeholder ends with `}`; options and markup are not supported");
        }
        return part;
    }

    // A pattern up to @p end (`}}` for a quoted one, the end of the text for a simple message).
    [[nodiscard]] std::expected<Message::Pattern, std::string> pattern(std::string_view end) {
        Message::Pattern out;
        std::string literal;
        while (!done() && !(!end.empty() && text.substr(at).starts_with(end))) {
            char const letter = peek();
            if (letter == '\\') {
                ++at;
                if (done() || (peek() != '{' && peek() != '}' && peek() != '|' && peek() != '\\')) {
                    return refuse("`\\` escapes only `{`, `}`, `|` and `\\`");
                }
                literal += peek();
                ++at;
                continue;
            }
            if (letter == '}') {
                return refuse("an unescaped `}`");
            }
            if (letter != '{') {
                literal += letter;
                ++at;
                continue;
            }
            ++at;
            std::expected<Message::Part, std::string> part = placeholder();
            if (!part) {
                return std::unexpected(std::move(part.error()));
            }
            if (!literal.empty()) {
                out.push_back(Message::Part{.text = std::move(literal), .variable = {}, .function = {}});
                literal.clear();
            }
            out.push_back(std::move(*part));
        }
        if (!end.empty() && !take(end)) {
            return refuse("a quoted pattern ends with `}}`");
        }
        if (!literal.empty()) {
            out.push_back(Message::Part{.text = std::move(literal), .variable = {}, .function = {}});
        }
        return out;
    }

    // NOLINTNEXTLINE(readability-function-cognitive-complexity): the complex message's grammar, in order.
    [[nodiscard]] std::expected<Message, std::string> complex() {
        Message message;
        skipSpace();
        while (take(".input")) {
            skipSpace();
            if (!take("{")) {
                return refuse("`.input` declares `{$name :number}` or `{$name :string}`");
            }
            std::expected<Message::Part, std::string> declared = placeholder();
            if (!declared) {
                return std::unexpected(std::move(declared.error()));
            }
            if (declared->function == Message::Function::None) {
                return refuse("`.input` declares `{$name :number}` or `{$name :string}`");
            }
            message.inputs.emplace_back(declared->variable, declared->function);
            skipSpace();
        }
        if (take(".local")) {
            return refuse("`.local` is not supported");
        }
        if (!take(".match")) {
            if (take("{{")) {
                std::expected<Message::Pattern, std::string> body = pattern("}}");
                if (!body) {
                    return std::unexpected(std::move(body.error()));
                }
                skipSpace();
                if (!done()) {
                    return refuse("nothing may follow the quoted pattern");
                }
                message.pattern = std::move(*body);
                return message;
            }
            return refuse("a complex message ends with `.match` or a quoted pattern `{{...}}`");
        }
        skipSpace();
        while (take("$")) {
            std::string selector = name();
            if (std::ranges::find(message.inputs, selector, &std::pair<std::string, Message::Function>::first) ==
                message.inputs.end()) {
                return refuse("`.match` selects on `$" + selector + "`, which no `.input` declares");
            }
            message.selectors.push_back(std::move(selector));
            skipSpace();
        }
        if (message.selectors.empty()) {
            return refuse("`.match` names at least one `$selector`");
        }
        bool catchAll = false;
        while (!done()) {
            Message::Variant variant;
            for (std::size_t key = 0; key < message.selectors.size(); ++key) {
                skipSpace();
                std::string word;
                if (take("*")) {
                    word = "*";
                } else if (take("|")) {
                    std::size_t const close = text.find('|', at);
                    if (close == std::string_view::npos) {
                        return refuse("a quoted key ends with `|`");
                    }
                    word = std::string{text.substr(at, close - at)};
                    at = close + 1;
                } else {
                    bool const exact = take("=");
                    word = (exact ? "=" : "") + name();
                    if (word.empty() || word == "=") {
                        return refuse("each variant has one key per selector");
                    }
                }
                variant.keys.push_back(std::move(word));
            }
            skipSpace();
            if (!take("{{")) {
                return refuse("a variant's pattern is quoted, `{{...}}`");
            }
            std::expected<Message::Pattern, std::string> body = pattern("}}");
            if (!body) {
                return std::unexpected(std::move(body.error()));
            }
            variant.pattern = std::move(*body);
            catchAll =
                catchAll || std::ranges::all_of(variant.keys, [](std::string const& key) { return key == "*"; });
            message.variants.push_back(std::move(variant));
            skipSpace();
        }
        if (!catchAll) {
            return refuse("`.match` needs a variant whose every key is `*`");
        }
        return message;
    }
};

}  // namespace detail

/// @brief Parses a message, refusing anything outside the subset `Message` describes.
/// @param text The message, as a catalogue or a `fallback` holds it.
/// @return The message, or why it is refused and where.
[[nodiscard]] inline std::expected<Message, std::string> parseMessage(std::string_view text) {
    detail::MessageReader reader{.text = text, .at = 0};
    std::string_view const trimmed = text.substr(std::min(text.find_first_not_of(" \t\r\n"), text.size()));
    if (trimmed.starts_with('.') || trimmed.starts_with("{{")) {
        return reader.complex();
    }
    Message message;
    std::expected<Message::Pattern, std::string> body = reader.pattern({});
    if (!body) {
        return std::unexpected(std::move(body.error()));
    }
    message.pattern = std::move(*body);
    return message;
}

/// @brief A message argument: a number with the places it shows, or text.
struct MessageArgument {
    /// @brief The number, when the argument is one.
    std::optional<math::Rational> number;
    /// @brief The places the number is shown with.
    std::uint32_t places = 0;
    /// @brief The text, when the argument is not a number.
    std::string text;
};

/// @brief Formats a parsed message.
/// @param message The message.
/// @param arguments The named arguments.
/// @param bcp47Locale The locale whose plural rules `.match` uses.
/// @param numeric The locale's digits and separators for `:number`.
/// @return The text, or why the message cannot be formatted with these arguments (a missing argument, a `:number`
///         given text).
[[nodiscard]] inline std::expected<std::string, std::string> formatMessage(
    Message const& message, std::vector<std::pair<std::string, MessageArgument>> const& arguments,
    std::string_view bcp47Locale, render::NumericLocale const& numeric) {
    auto const argument = [&arguments](std::string const& name) -> MessageArgument const* {
        auto const found = std::ranges::find(arguments, name, &std::pair<std::string, MessageArgument>::first);
        return found == arguments.end() ? nullptr : &found->second;
    };
    auto const declared = [&message](std::string const& name, Message::Function written) {
        auto const found = std::ranges::find(message.inputs, name, &std::pair<std::string, Message::Function>::first);
        return found == message.inputs.end() || written != Message::Function::None ? written : found->second;
    };
    auto const write = [&](Message::Pattern const& pattern) -> std::expected<std::string, std::string> {
        std::string out;
        for (Message::Part const& part : pattern) {
            if (part.variable.empty()) {
                out += part.text;
                continue;
            }
            MessageArgument const* const value = argument(part.variable);
            if (value == nullptr) {
                return std::unexpected("no argument `" + part.variable + "`");
            }
            Message::Function const function = declared(part.variable, part.function);
            if (value->number && function != Message::Function::String) {
                out += render::formatCanonicalNumber(decimalText(*value->number, value->places), numeric);
            } else if (function == Message::Function::Number) {
                return std::unexpected("`" + part.variable + "` is formatted `:number` but is not a number");
            } else {
                out += value->number ? decimalText(*value->number, value->places) : value->text;
            }
        }
        return out;
    };
    if (message.selectors.empty()) {
        return write(message.pattern);
    }
    // Each selector's candidate keys, best first: an exact value, then its plural category (or its text).
    std::vector<std::vector<std::string>> wanted;
    for (std::string const& selector : message.selectors) {
        MessageArgument const* const value = argument(selector);
        if (value == nullptr) {
            return std::unexpected("no argument `" + selector + "`");
        }
        if (declared(selector, Message::Function::None) == Message::Function::Number) {
            if (!value->number) {
                return std::unexpected("`" + selector + "` selects `:number` but is not a number");
            }
            std::string const shown = decimalText(*value->number, value->places);
            wanted.push_back(
                {shown, "=" + shown, std::string{pluralKeyword(pluralCategory(bcp47Locale, pluralOperands(shown)))}});
        } else {
            wanted.push_back({value->number ? decimalText(*value->number, value->places) : value->text});
        }
    }
    // The first variant whose every key matches, the best match per selector first: MessageFormat 2 orders
    // variants by key preference, and a catalogue lists exact values before categories, categories before `*`.
    Message::Variant const* best = nullptr;
    std::vector<std::size_t> bestRank;
    for (Message::Variant const& variant : message.variants) {
        std::vector<std::size_t> rank;
        bool matches = true;
        for (std::size_t index = 0; index < variant.keys.size() && matches; ++index) {
            std::string const& key = variant.keys.at(index);
            if (key == "*") {
                rank.push_back(wanted.at(index).size());
                continue;
            }
            auto const found = std::ranges::find(wanted.at(index), key);
            matches = found != wanted.at(index).end();
            rank.push_back(static_cast<std::size_t>(found - wanted.at(index).begin()));
        }
        if (matches && (best == nullptr || rank < bestRank)) {
            best = &variant;
            bestRank = std::move(rank);
        }
    }
    return write(best->pattern);
}

}  // namespace morph::ui

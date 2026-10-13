// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file forms/engine/values.hpp
/// @brief The forms engine's encoders and decoders: a field's draft text to its exact wire
///        literal, a wire value back to the draft a control shows, exact unit conversion, and
///        Choice option rows.
///
/// Specified in `docs/spec/forms/engine.md`, "Values and exactness".

#include <algorithm>
#include <array>
#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../../render/i18n.hpp"
#include "../../render/locale_format.hpp"
#include "../../ui/value.hpp"
#include "../../util/datetime.hpp"
#include "../../util/rational.hpp"
#include "detail/text.hpp"
#include "field_model.hpp"
#include "rules.hpp"

namespace morph::forms {

/// @brief Why a draft does not encode: a stable code and the message shown beside the field.
struct FieldError {
    /// @brief One of `malformed`, `too-precise`, `below-minimum`, `above-maximum`, `not-a-multiple`,
    ///        `not-an-option`, `out-of-range`, `incomplete`, `unrepresentable`.
    std::string code{};
    /// @brief The message, resolved through the session's translations
    ///        (key `morph.forms.error.` followed by the code), else the English text.
    std::string message{};
    /// @brief Member-wise equality.
    /// @param other The other error.
    /// @return `true` when code and message match.
    bool operator==(FieldError const& other) const = default;
};

/// @brief One field's encoded value.
struct WireValue {
    /// @brief The JSON literal the body carries, byte for byte.
    std::string json{};
    /// @brief The exact value rules compare, in the expression language's terms: an `int`, a `decimal`, a `bool`,
    ///        a `string` or a `timestamp`; `null` for objects and for a value the language cannot hold exactly
    ///        (an integer beyond int64).
    ui::Value value{};
    /// @brief Member-wise equality.
    /// @param other The other value.
    /// @return `true` when literal and value match.
    bool operator==(WireValue const& other) const = default;
};

/// @brief A draft's encoding: `std::nullopt` when blank, else the value or why it does not encode.
using Encoding = std::optional<std::expected<WireValue, FieldError>>;

/// @brief The session options a value conversion reads.
struct ValueContext {
    /// @brief How numbers are typed and shown. The views must outlive the context.
    render::NumericLocale locale{};
    /// @brief The display zone for date-times, in minutes east of UTC.
    int displayOffsetMinutes = 0;
    /// @brief The host's catalog, for error messages; empty means none.
    render::TranslationProvider translations{};
    /// @brief The locale to resolve messages in.
    std::string bcp47 = "en";
};

/// @brief The error for @p code, its message resolved through @p context's translations.
/// @param code    A `FieldError::code`.
/// @param context The session's value context.
/// @return The error.
[[nodiscard]] inline FieldError fieldError(std::string_view code, ValueContext const& context) {
    constexpr std::array<std::pair<std::string_view, std::string_view>, 9> kEnglish{{
        {"malformed", "Not a valid value"},
        {"too-precise", "Too many decimal places"},
        {"below-minimum", "Below the minimum"},
        {"above-maximum", "Above the maximum"},
        {"not-a-multiple", "Not an allowed step"},
        {"not-an-option", "Not one of the options"},
        {"out-of-range", "Too large to send exactly"},
        {"incomplete", "A required part is missing"},
        {"unrepresentable", "Nested too deeply to edit here"},
    }};
    auto const found = std::ranges::find(kEnglish, code, &std::pair<std::string_view, std::string_view>::first);
    std::string_view const english = found == kEnglish.end() ? std::string_view{"Not a valid value"} : found->second;
    std::string const key = "morph.forms.error." + std::string{code};
    return FieldError{std::string{code},
                      render::resolveText(context.translations, context.bcp47, std::nullopt, key, english)};
}

namespace detail::engine {

/// @brief The value a JSON literal denotes, for rules: what `ui::valueFromJson` reads, and a number with a fraction
///        as the exact decimal its digits spell (never through a double's value).
/// @param literal The literal.
/// @return The value; `null` for what the expression language cannot hold exactly.
[[nodiscard]] inline ui::Value valueOfLiteral(Json const& literal) {
    if (literal.get_if<double>() != nullptr) {
        auto const text = numberText(literal);
        auto const exact = text ? render::parseDecimal(*text, render::NumericLocale{}) : std::nullopt;
        return exact ? ui::Value{ui::Decimal{*exact}} : ui::Value{};
    }
    auto value = ui::valueFromJson(literal);
    return value ? *std::move(value) : ui::Value{};
}

/// @brief The `int` canonical integer digits spell.
/// @param digits Canonical integer text.
/// @return The value; `null` beyond int64.
[[nodiscard]] inline ui::Value integerValue(std::string_view digits) {
    auto const exact = render::parseDecimal(digits, render::NumericLocale{});
    return exact && exact->isInteger() ? ui::Value{exact->numerator} : ui::Value{};
}

/// @brief The `decimal` canonical decimal text spells.
/// @param canonical Canonical decimal text.
/// @return The value; `null` beyond what `math::Rational` holds.
[[nodiscard]] inline ui::Value decimalValue(std::string_view canonical) {
    auto const exact = render::parseDecimal(canonical, render::NumericLocale{});
    return exact ? ui::Value{ui::Decimal{*exact}} : ui::Value{};
}

/// @brief Whether @p value is a whole multiple of @p step, exactly.
/// @param value The value.
/// @param step  The step; positive.
/// @return `false` as well when the quotient does not fit `math::Rational`.
[[nodiscard]] inline bool isMultiple(math::Rational const& value, math::Rational const& step) {
    auto const quotient = math::checkedDiv(value, step);
    return quotient.has_value() && quotient->isInteger();
}

/// @brief A canonical decimal in @p locale's spelling, without grouping — what a control is seeded with.
/// @param canonical Canonical decimal text.
/// @param locale    The display locale.
/// @return The draft text.
[[nodiscard]] inline std::string draftNumber(std::string_view canonical, render::NumericLocale const& locale) {
    render::NumericLocale ungrouped = locale;
    ungrouped.groupSeparator = {};
    return render::formatCanonicalNumber(canonical, ungrouped);
}

/// @brief Checks @p canonical against a field's `minimum`, `maximum` and `multipleOf`.
/// @param spec      The field.
/// @param canonical The value as canonical decimal text.
/// @param context   For the message.
/// @return The violation, or `std::nullopt`.
[[nodiscard]] inline std::optional<FieldError> decimalBounds(FieldSpec const& spec, std::string_view canonical,
                                                             ValueContext const& context) {
    if (spec.minimum && compareDecimal(canonical, *spec.minimum) == std::strong_ordering::less) {
        return fieldError("below-minimum", context);
    }
    if (spec.maximum && compareDecimal(canonical, *spec.maximum) == std::strong_ordering::greater) {
        return fieldError("above-maximum", context);
    }
    if (spec.multipleOf) {
        auto const value = render::parseDecimal(canonical, render::NumericLocale{});
        auto const step = render::parseDecimal(*spec.multipleOf, render::NumericLocale{});
        if (!value || !step || !isMultiple(*value, *step)) {
            return fieldError("not-a-multiple", context);
        }
    }
    return std::nullopt;
}

/// @brief Encodes an integer draft.
/// @param spec    The field.
/// @param text    The trimmed draft.
/// @param context For messages.
/// @return The encoding.
[[nodiscard]] inline Encoding encodeInteger(FieldSpec const& spec, std::string_view text,
                                            ValueContext const& context) {
    if (!isCanonicalInteger(text)) {
        return std::unexpected(fieldError("malformed", context));
    }
    std::string const normalised = stripLeadingZeros(text);
    auto const lower = spec.exactMinimum ? spec.exactMinimum : spec.minimum;
    auto const upper = spec.exactMaximum ? spec.exactMaximum : spec.maximum;
    if (lower && compareDecimal(normalised, *lower) == std::strong_ordering::less) {
        return std::unexpected(fieldError("below-minimum", context));
    }
    if (upper && compareDecimal(normalised, *upper) == std::strong_ordering::greater) {
        return std::unexpected(fieldError("above-maximum", context));
    }
    if (spec.multipleOf) {
        if (auto const violation = decimalBounds(FieldSpec{.multipleOf = spec.multipleOf}, normalised, context)) {
            return std::unexpected(*violation);
        }
    }
    return WireValue{normalised, integerValue(normalised)};
}

/// @brief Encodes a plain JSON number draft.
/// @param spec    The field.
/// @param text    The trimmed draft.
/// @param context The locale and messages.
/// @return The encoding.
[[nodiscard]] inline Encoding encodeNumber(FieldSpec const& spec, std::string_view text, ValueContext const& context) {
    auto const canonical = render::normalizeLocaleNumber(text, context.locale);
    if (!canonical || !isCanonicalDecimal(*canonical)) {
        return std::unexpected(fieldError("malformed", context));
    }
    auto const dot = canonical->find('.');
    std::size_t const places = dot == std::string::npos ? 0 : canonical->size() - dot - 1;
    if (spec.displayDecimals && places > *spec.displayDecimals) {
        return std::unexpected(fieldError("too-precise", context));
    }
    if (auto const violation = decimalBounds(spec, *canonical, context)) {
        return std::unexpected(*violation);
    }
    std::string const literal = stripLeadingZeros(*canonical);
    return WireValue{literal, decimalValue(literal)};
}

/// @brief Encodes a Decimal or Quantity draft typed in `spec.units[unit]` as the exact
///        `{num, den, dp}` in the canonical unit, unreduced: the scaled digits times the unit's
///        numerator over `10^decimals` times its denominator.
/// @param spec    The field.
/// @param text    The trimmed draft.
/// @param unit    Index into `spec.units`.
/// @param context The locale and messages.
/// @return The encoding.
[[nodiscard]] inline Encoding encodeExact(FieldSpec const& spec, std::string_view text, std::size_t unit,
                                          ValueContext const& context) {
    UnitOption const fallback{spec.unitAscii, spec.unit, spec.decimalPlaces.value_or(0), 1, 1};
    UnitOption const& entry = unit < spec.units.size() ? spec.units.at(unit) : fallback;
    auto const canonical = render::normalizeLocaleNumber(text, context.locale);
    if (!canonical || !isCanonicalDecimal(*canonical)) {
        return std::unexpected(fieldError("malformed", context));
    }
    auto const dot = canonical->find('.');
    std::size_t const places = dot == std::string::npos ? 0 : canonical->size() - dot - 1;
    if (places > entry.decimals) {
        return std::unexpected(fieldError("too-precise", context));
    }
    auto const typed = render::parseDecimal(*canonical, render::NumericLocale{}, entry.decimals);
    std::int64_t const scale = math::detail::powerOfTen(entry.decimals);
    if (!typed || scale == 0) {
        return std::unexpected(fieldError("out-of-range", context));
    }
    std::int64_t const factor = scale / typed->denominator;
    if (math::detail::mulOverflows(typed->numerator, factor)) {
        return std::unexpected(fieldError("out-of-range", context));
    }
    std::int64_t const scaled = typed->numerator * factor;
    if (math::detail::mulOverflows(scaled, entry.num) || math::detail::mulOverflows(scale, entry.den)) {
        return std::unexpected(fieldError("out-of-range", context));
    }
    std::int64_t const num = scaled * entry.num;
    std::int64_t const den = scale * entry.den;
    std::uint32_t const canonDp = spec.decimalPlaces.value_or(0);
    math::Rational const value{math::Numerator{num}, math::Denominator{den}, math::DecimalPlaces{canonDp}};
    if (spec.minimum && compareRationalToDecimal(value, *spec.minimum) == std::strong_ordering::less) {
        return std::unexpected(fieldError("below-minimum", context));
    }
    if (spec.maximum && compareRationalToDecimal(value, *spec.maximum) == std::strong_ordering::greater) {
        return std::unexpected(fieldError("above-maximum", context));
    }
    if (spec.instanceMinimum && value < *spec.instanceMinimum) {
        return std::unexpected(fieldError("below-minimum", context));
    }
    if (spec.instanceMaximum && value > *spec.instanceMaximum) {
        return std::unexpected(fieldError("above-maximum", context));
    }
    if (spec.multipleOf) {
        auto const step = render::parseDecimal(*spec.multipleOf, render::NumericLocale{});
        if (!step || !isMultiple(value, *step)) {
            return std::unexpected(fieldError("not-a-multiple", context));
        }
    }
    std::string const json = R"({"num":)" + std::to_string(num) + R"(,"den":)" + std::to_string(den) + R"(,"dp":)" +
                             std::to_string(canonDp) + "}";
    return WireValue{json, ui::Value{ui::Decimal{value}}};
}

/// @brief Encodes a comma-separated array draft by the item type.
/// @param spec    The field.
/// @param text    The trimmed draft.
/// @param context For messages.
/// @return The encoding; blank items are dropped, so `" , "` is `[]`.
[[nodiscard]] inline Encoding encodeArray(FieldSpec const& spec, std::string_view text, ValueContext const& context) {
    std::string json = "[";
    bool first = true;
    std::size_t start = 0;
    while (start <= text.size()) {
        auto const comma = text.find(',', start);
        std::string_view const item =
            trim(text.substr(start, comma == std::string_view::npos ? text.npos : comma - start));
        start = comma == std::string_view::npos ? text.size() + 1 : comma + 1;
        if (item.empty()) {
            continue;
        }
        std::string literal;
        if (spec.itemType == "integer") {
            if (!isCanonicalInteger(item)) {
                return std::unexpected(fieldError("malformed", context));
            }
            literal = stripLeadingZeros(item);
        } else if (spec.itemType == "number") {
            if (!isCanonicalDecimal(item)) {
                return std::unexpected(fieldError("malformed", context));
            }
            literal = stripLeadingZeros(item);
        } else if (spec.itemType == "boolean") {
            if (item != "true" && item != "false") {
                return std::unexpected(fieldError("malformed", context));
            }
            literal = std::string{item};
        } else {
            literal = quote(item);
        }
        json += (first ? "" : ",") + literal;
        first = false;
    }
    json += "]";
    auto const parsed = parseJson(json);
    return WireValue{json, parsed ? valueOfLiteral(*parsed) : ui::Value{}};
}

}  // namespace detail::engine

/// @brief Encodes one scalar field's draft (every kind but `Object` and `ObjectArray`).
///
/// The draft is trimmed first. Blank is `std::nullopt` — except a required Boolean, which encodes
/// `false`: an unchecked box is an answer, and a required box always shows one.
/// @param spec    The field.
/// @param draft   What the user typed, or what a control holds (an option's `valueJson`, `true`/`false`).
/// @param unit    For a Decimal or Quantity, the index into `spec.units` the draft is typed in.
/// @param context The locale, display zone and translations.
/// @return The encoding.
[[nodiscard]] inline Encoding encodeScalar(FieldSpec const& spec, std::string_view draft, std::size_t unit,
                                           ValueContext const& context) {
    using namespace detail::engine;
    std::string_view const text = trim(draft);
    if (text.empty()) {
        if (spec.kind == FieldKind::Boolean && spec.required) {
            return WireValue{"false", ui::Value{false}};
        }
        return std::nullopt;
    }
    switch (spec.kind) {
        case FieldKind::Text:
        case FieldKind::Multiline:
            return WireValue{quote(text), ui::Value{std::string{text}}};
        case FieldKind::Date:
            if (!isIsoDate(text)) {
                return std::unexpected(fieldError("malformed", context));
            }
            return WireValue{quote(text), ui::Value{std::string{text}}};
        case FieldKind::DateTime: {
            auto const wall = parseWallClock(text);
            if (!wall) {
                return std::unexpected(fieldError("malformed", context));
            }
            std::string const utc = formatUtc(*wall - std::chrono::minutes{context.displayOffsetMinutes});
            auto const instant = time::DateTime::fromIso8601(utc);
            return WireValue{quote(utc), instant ? ui::Value{*instant} : ui::Value{}};
        }
        case FieldKind::EpochDateTime: {
            auto const wall = parseWallClock(text);
            if (!wall) {
                return std::unexpected(fieldError("malformed", context));
            }
            auto const seconds =
                (*wall - std::chrono::minutes{context.displayOffsetMinutes}).time_since_epoch().count();
            return WireValue{std::to_string(seconds), ui::Value{static_cast<std::int64_t>(seconds)}};
        }
        case FieldKind::EpochDate: {
            if (!isIsoDate(text)) {
                return std::unexpected(fieldError("malformed", context));
            }
            auto const days = std::chrono::floor<std::chrono::days>(*parseWallClock(std::string{text} + "T00:00"))
                                  .time_since_epoch()
                                  .count();
            return WireValue{std::to_string(days), ui::Value{static_cast<std::int64_t>(days)}};
        }
        case FieldKind::Boolean:
            if (text == "true" || text == "false") {
                return WireValue{std::string{text}, ui::Value{text == "true"}};
            }
            return std::unexpected(fieldError("malformed", context));
        case FieldKind::Enum: {
            auto const found = std::ranges::find(spec.enumOptions, text, &ChoiceOption::valueJson);
            if (found == spec.enumOptions.end()) {
                return std::unexpected(fieldError("not-an-option", context));
            }
            auto const literal = parseJson(text);
            return WireValue{std::string{text}, literal ? valueOfLiteral(*literal) : ui::Value{}};
        }
        case FieldKind::Choice: {
            auto const literal = parseJson(text);
            if (!literal || literal->is_object() || literal->is_array() || literal->is_null()) {
                return std::unexpected(fieldError("malformed", context));
            }
            return WireValue{std::string{text}, valueOfLiteral(*literal)};
        }
        case FieldKind::Integer:
            return encodeInteger(spec, text, context);
        case FieldKind::Slider:
            return spec.jsonType == "number" ? encodeNumber(spec, text, context) : encodeInteger(spec, text, context);
        case FieldKind::Number:
            return encodeNumber(spec, text, context);
        case FieldKind::Decimal:
        case FieldKind::Quantity:
            return encodeExact(spec, text, unit, context);
        case FieldKind::Array:
            return encodeArray(spec, text, context);
        case FieldKind::Object:
        case FieldKind::ObjectArray:
        default:
            return std::unexpected(fieldError("unrepresentable", context));
    }
}

/// @brief The draft a control shows for the wire value @p value — the inverse of `encodeScalar`,
///        so encoding the result re-encodes @p value (numbers in the display locale, without
///        grouping; instants in the display zone).
/// @param spec    The field.
/// @param value   The wire value (JSON null and a mismatched shape decode to blank).
/// @param context The locale and display zone.
/// @return The draft text.
[[nodiscard]] inline std::string decodeScalar(FieldSpec const& spec, detail::engine::Json const& value,
                                              ValueContext const& context) {
    using namespace detail::engine;
    if (value.is_null()) {
        return {};
    }
    switch (spec.kind) {
        case FieldKind::Text:
        case FieldKind::Multiline:
        case FieldKind::Date: {
            auto const* const text = value.get_if<std::string>();
            return text == nullptr ? std::string{} : *text;
        }
        case FieldKind::DateTime: {
            auto const* const text = value.get_if<std::string>();
            auto const instant = text == nullptr ? std::nullopt : parseInstant(*text);
            return instant ? formatWallClock(*instant + std::chrono::minutes{context.displayOffsetMinutes})
                           : std::string{};
        }
        case FieldKind::EpochDateTime:
        case FieldKind::EpochDate: {
            auto const count = SchemaReader::signedOf(&value);
            if (!count || value.get_if<double>() != nullptr) {
                return {};
            }
            if (spec.kind == FieldKind::EpochDate) {
                return formatWallClock(std::chrono::sys_seconds{std::chrono::days{*count}}).substr(0, 10);
            }
            return formatWallClock(std::chrono::sys_seconds{std::chrono::seconds{*count}} +
                                   std::chrono::minutes{context.displayOffsetMinutes});
        }
        case FieldKind::Boolean: {
            auto const* const flag = value.get_if<bool>();
            return flag == nullptr ? std::string{} : (*flag ? "true" : "false");
        }
        case FieldKind::Enum:
        case FieldKind::Choice:
            return toJson(value);
        case FieldKind::Integer:
        case FieldKind::Slider:
            return numberText(value).value_or(std::string{});
        case FieldKind::Number: {
            auto canonical = numberText(value);
            if (!canonical) {
                return {};
            }
            if (spec.displayDecimals) {
                auto const dot = canonical->find('.');
                std::size_t const places = dot == std::string::npos ? 0 : canonical->size() - dot - 1;
                if (places < *spec.displayDecimals) {
                    if (dot == std::string::npos) {
                        *canonical += '.';
                    }
                    canonical->append(*spec.displayDecimals - places, '0');
                }
            }
            return draftNumber(*canonical, context.locale);
        }
        case FieldKind::Decimal:
        case FieldKind::Quantity: {
            auto const num = SchemaReader::signedOf(member(value, "num"));
            auto const den = SchemaReader::signedOf(member(value, "den"));
            if (!num || !den || *den == 0) {
                return {};
            }
            math::Rational const exact{math::Numerator{*num}, math::Denominator{*den}, math::DecimalPlaces{0}};
            return draftNumber(decimalText(exact, spec.decimalPlaces.value_or(0)), context.locale);
        }
        case FieldKind::Array: {
            if (!value.is_array()) {
                return {};
            }
            std::string out;
            for (Json const& item : value.get_array()) {
                out += (out.empty() ? "" : ", ") + displayText(item);
            }
            return out;
        }
        case FieldKind::Object:
        case FieldKind::ObjectArray:
        default:
            return {};
    }
}

/// @brief Converts a draft from one unit to another exactly, rounding half away from zero to
///        the target unit's decimals.
/// @param draft  The draft, in @p locale's spelling.
/// @param from   The unit it is typed in.
/// @param target The unit to convert to.
/// @param locale The display locale (the result is grouped in it).
/// @return The converted draft, or `std::nullopt` when the draft is not a number or the
///         conversion leaves the exact range.
[[nodiscard]] inline std::optional<std::string> convertDraft(std::string_view draft, UnitOption const& from,
                                                             UnitOption const& target,
                                                             render::NumericLocale const& locale) {
    auto const typed = render::parseDecimal(detail::engine::trim(draft), locale);
    if (!typed || from.den == 0 || target.num == 0) {
        return std::nullopt;
    }
    math::Rational const toCanonical{math::Numerator{from.num}, math::Denominator{from.den}, math::DecimalPlaces{0}};
    math::Rational const fromCanonical{math::Numerator{target.den}, math::Denominator{target.num},
                                       math::DecimalPlaces{0}};
    auto const canonical = math::checkedMul(*typed, toCanonical);
    auto const converted = canonical ? math::checkedMul(*canonical, fromCanonical) : canonical;
    if (!converted) {
        return std::nullopt;
    }
    return render::formatCanonicalNumber(detail::engine::decimalText(*converted, target.decimals), locale);
}

/// @brief The options an options action's reply carries: the reply itself when it is an array,
///        else its first array-valued member; rows without the value member are skipped.
/// @param reply The reply JSON.
/// @param spec  Which row members are value and label.
/// @return The options; empty for a reply that is not JSON or has no rows.
[[nodiscard]] inline std::vector<ChoiceOption> parseOptions(std::string_view reply, ChoiceSpec const& spec) {
    using namespace detail::engine;
    std::vector<ChoiceOption> options{};
    auto const dom = parseJson(reply);
    if (!dom) {
        return options;
    }
    Json const* rows = dom->is_array() ? &*dom : nullptr;
    if (rows == nullptr && dom->is_object()) {
        for (auto const& [key, candidate] : dom->get_object()) {
            if (candidate.is_array()) {
                rows = &candidate;
                break;
            }
        }
    }
    if (rows == nullptr) {
        return options;
    }
    for (Json const& row : rows->get_array()) {
        Json const* const value = member(row, spec.valueField);
        if (value == nullptr) {
            continue;
        }
        Json const* const label = member(row, spec.labelField);
        options.push_back(ChoiceOption{toJson(*value), displayText(label != nullptr ? *label : *value)});
    }
    return options;
}

/// @brief The placeholder a control shows when the schema declares none.
/// @param spec The field.
/// @return `x-placeholder` when declared; else `0.000…` for a Decimal or Quantity (at least one
///         place), `0` for an integer, the declared precision for a Number, a hint for an Array,
///         and empty otherwise.
[[nodiscard]] inline std::string defaultPlaceholder(FieldSpec const& spec) {
    if (!spec.placeholder.empty()) {
        return spec.placeholder;
    }
    switch (spec.kind) {
        case FieldKind::Decimal:
        case FieldKind::Quantity:
            return "0." + std::string(std::max<std::uint32_t>(1, spec.decimalPlaces.value_or(0)), '0');
        case FieldKind::Integer:
        case FieldKind::Slider:
            return "0";
        case FieldKind::Number:
            if (!spec.displayDecimals) {
                return {};
            }
            return *spec.displayDecimals == 0 ? std::string{"0"} : "0." + std::string(*spec.displayDecimals, '0');
        case FieldKind::Array:
            return "comma-separated (e.g. red, green, blue)";
        case FieldKind::Text:
        case FieldKind::Multiline:
        case FieldKind::Boolean:
        case FieldKind::Enum:
        case FieldKind::Choice:
        case FieldKind::DateTime:
        case FieldKind::Date:
        case FieldKind::EpochDateTime:
        case FieldKind::EpochDate:
        case FieldKind::Object:
        case FieldKind::ObjectArray:
        default:
            return {};
    }
}

}  // namespace morph::forms

// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file ui/measure.hpp
/// @brief What a user types for an amount with a unit, and the operand a filter takes for it.
///
/// A client parses a measure with the units of the table's description (`TableDescription::units`) and its locale's
/// number, and sends the wire's operand: a decimal with a point, a space and a unit id, whatever the locale. See
/// `docs/spec/table/table.md`, "Filters on values with units".

#include <cstddef>
#include <morph/table/data_source.hpp>
#include <morph/ui/locale_table.hpp>
#include <morph/util/quantity.hpp>
#include <morph/util/rational.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace morph::ui {

/// @brief An amount a user typed, with the unit it names.
struct TypedMeasure {
    /// @brief The amount, exact.
    math::Rational amount;
    /// @brief The unit's id; empty is the ratio's unit.
    std::string unit;

    /// @brief Member-wise equality, the amount by value.
    /// @param other The measure to compare with.
    /// @return `true` when the amounts are equal and the units are the same id.
    [[nodiscard]] bool operator==(TypedMeasure const& other) const = default;
};

namespace detail {

/// @brief Whether two texts are equal with ASCII letters compared without regard to case.
/// @param left  One text.
/// @param right The other.
/// @return `true` when they differ only in the case of ASCII letters.
[[nodiscard]] inline bool equalsIgnoringAsciiCase(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    auto const lower = [](char character) {
        return character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a') : character;
    };
    for (std::size_t at = 0; at < left.size(); ++at) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): `at` is below both sizes, which are equal; `.at()` could throw out of a noexcept function.
        if (lower(left[at]) != lower(right[at])) {
            return false;
        }
    }
    return true;
}

}  // namespace detail

/// @brief Reads what a user typed for a measure: the locale's number, optional spaces, then a unit.
///
/// The number is an optional minus sign (`-` or the locale's), ASCII digits, and at most one decimal separator of
/// the locale followed by digits; a group separator is not read. The unit is the rest of the text, matched against a
/// unit's `symbol` or `id` exactly and then without regard to the case of ASCII letters, the first match in
/// @p units order winning each pass. Text with no unit takes @p chosen, the unit a menu shows as chosen.
/// @param text   What the user typed; spaces around it are ignored.
/// @param units  The units the user may name: the table's description's.
/// @param locale The locale the user types in.
/// @param chosen The unit's id for text with no unit; empty is the ratio's unit.
/// @return The amount and the unit's id, or `nullopt` when the text is not a number and a unit of @p units.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): reads sign, digits, separator, digits and unit in one left-to-right pass over the text with a running index
[[nodiscard]] inline std::optional<TypedMeasure> parseMeasure(std::string_view text,
                                                              std::span<table::UnitInfo const> units,
                                                              LocaleFacts const& locale,
                                                              std::string_view chosen = {}) {
    auto const first = text.find_first_not_of(' ');
    if (first == std::string_view::npos) {
        return std::nullopt;
    }
    text = text.substr(first, text.find_last_not_of(' ') - first + 1);
    std::string canonical;
    std::size_t index = 0;
    auto const negative = locale.numeric.negativeSign.empty() ? std::string_view{"-"} : locale.numeric.negativeSign;
    if (text.starts_with('-')) {
        canonical += '-';
        index = 1;
    } else if (text.starts_with(negative)) {
        canonical += '-';
        index = negative.size();
    }
    auto const digits = [&] {
        auto const from = index;
        while (index < text.size() && text.at(index) >= '0' && text.at(index) <= '9') {
            canonical += text.at(index++);
        }
        return index > from;
    };
    if (!digits()) {
        return std::nullopt;
    }
    auto const separator = locale.numeric.decimalSeparator;
    if (!separator.empty() && text.substr(index).starts_with(separator)) {
        index += separator.size();
        canonical += '.';
        if (!digits()) {
            return std::nullopt;
        }
    }
    auto const amount = table::parseDecimal(canonical);
    if (!amount) {
        return std::nullopt;
    }
    while (index < text.size() && text.at(index) == ' ') {
        ++index;
    }
    auto const named = text.substr(index);
    if (named.empty()) {
        return TypedMeasure{.amount = *amount, .unit = std::string{chosen}};
    }
    for (auto const exact : {true, false}) {
        for (auto const& unit : units) {
            auto const matches = [&](std::string_view name) {
                return !name.empty() && (exact ? name == named : detail::equalsIgnoringAsciiCase(name, named));
            };
            if (matches(unit.symbol) || matches(unit.id)) {
                return TypedMeasure{.amount = *amount, .unit = unit.id};
            }
        }
    }
    return std::nullopt;
}

/// @brief The operand a filter takes for a measure: the wire's form, whatever the user's locale.
/// @param measure The measure.
/// @return The amount as a decimal with a point, then a space and the unit's id: `"0.3 kg"`; the amount alone for
///         the ratio's unit, `"0.003"`.
[[nodiscard]] inline std::string measureOperand(TypedMeasure const& measure) {
    auto out = units::detail::formatRationalDecimal(measure.amount);
    if (!measure.unit.empty()) {
        out += ' ';
        out += measure.unit;
    }
    return out;
}

}  // namespace morph::ui

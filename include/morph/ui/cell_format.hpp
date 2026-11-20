// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file ui/cell_format.hpp
/// @brief How a table column shows its cells: the one formatter every backend, the copy, the status line and the
///        filter menu call.
///
/// A cell crosses the wire as its exact value; the client turns it into text here, in the session's locale, by the
/// column's `table::ColumnDisplay`. Order, filters and distinct values always use the exact value, never this text.
///
/// See `docs/spec/table/table.md`, "How a column shows its cells" and "Values with units".

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <map>
#include <morph/render/locale_format.hpp>
#include <morph/table/data_source.hpp>
#include <morph/table/description.hpp>
#include <morph/table/query.hpp>
#include <morph/ui/date_pattern.hpp>
#include <morph/ui/expression.hpp>
#include <morph/ui/locale_table.hpp>
#include <morph/util/datetime.hpp>
#include <morph/util/quantity.hpp>
#include <morph/util/rational.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace morph::ui {

/// @brief How a cell draws, beside its text.
enum class CellLook : std::uint8_t {
    Text,      ///< The cell's text: its value's, or a label's.
    Empty,     ///< An empty cell: the column's `empty` text.
    Unloaded,  ///< A value that has not arrived: the backend draws its placeholder, and the text is empty.
    Invalid,   ///< A value that cannot be read as the column's kind: an error mark, and the text is empty.
};

/// @brief What a `Measure` cell is shown through, which the column alone does not carry.
struct DisplayUnits {
    /// @brief The table's units, not owned; null shows every `Measure` cell as `Invalid`.
    table::UnitRegistry const* units = nullptr;
    /// @brief Per dimension, the unit id the user chose to show it in; a dimension not named shows each cell in its
    ///        own unit.
    std::map<std::string, std::string, std::less<>> byDimension;
};

namespace detail::cells {

/// @brief A number as canonical decimal text, split: its sign, its whole digits and its fraction digits.
struct Digits {
    /// @brief Whether the number is below zero.
    bool negative = false;
    /// @brief The digits before the point; at least one.
    std::string whole;
    /// @brief The digits after the point; empty for none.
    std::string fraction;
};

/// @brief Reads canonical `-?digits(.digits)?` text.
/// @param text The text.
/// @return The parts; nothing for text of another shape.
[[nodiscard]] inline std::optional<Digits> digitsOf(std::string_view text) {
    Digits out;
    if (text.starts_with('-')) {
        out.negative = true;
        text.remove_prefix(1);
    }
    auto const dot = text.find('.');
    out.whole = std::string{text.substr(0, dot)};
    if (dot != std::string_view::npos) {
        out.fraction = std::string{text.substr(dot + 1)};
    }
    auto const digit = [](char c) { return c >= '0' && c <= '9'; };
    if (out.whole.empty() || !std::ranges::all_of(out.whole, digit) || !std::ranges::all_of(out.fraction, digit) ||
        (dot != std::string_view::npos && out.fraction.empty())) {
        return std::nullopt;
    }
    return out;
}

/// @brief Canonical text of @p digits: no sign on a value whose digits are all zero.
/// @param digits The parts.
/// @return The text.
[[nodiscard]] inline std::string textOf(Digits const& digits) {
    bool const zero = std::ranges::all_of(digits.whole, [](char c) { return c == '0'; }) &&
                      std::ranges::all_of(digits.fraction, [](char c) { return c == '0'; });
    std::string out = digits.negative && !zero ? "-" : "";
    out += digits.whole;
    if (!digits.fraction.empty()) {
        out += '.';
        out += digits.fraction;
    }
    return out;
}

/// @brief Rounds canonical text to @p places fraction digits, then pads or trims it.
///
/// The dropped tail decides: above half rounds the magnitude up, below keeps it, and exactly half rounds up under
/// `HalfUp` and to the even last kept digit under `HalfEven`. With @p pad the fraction is padded to @p places;
/// without, trailing zeros and a bare point go.
/// @param digits   The number.
/// @param places   The places kept.
/// @param rounding The rule for a half.
/// @param pad      Whether fewer places are padded with zeros.
/// @return The canonical text.
[[nodiscard]] inline std::string roundText(Digits digits, std::size_t places, table::Rounding rounding, bool pad) {
    if (digits.fraction.size() > places) {
        std::string_view const dropped = std::string_view{digits.fraction}.substr(places);
        bool const restZero = std::ranges::all_of(dropped.substr(1), [](char c) { return c == '0'; });
        char const first = dropped.front();
        digits.fraction.resize(places);
        bool up = false;
        if (first > '5' || (first == '5' && !restZero)) {
            up = true;
        } else if (first == '5') {
            char const last = places > 0 ? digits.fraction.back() : digits.whole.back();
            up = rounding == table::Rounding::HalfUp || ((last - '0') % 2) != 0;
        }
        if (up) {
            std::string all = digits.whole + digits.fraction;
            std::size_t at = all.size();
            while (at > 0) {
                --at;
                if (all.at(at) == '9') {
                    all.at(at) = '0';
                } else {
                    ++all.at(at);
                    break;
                }
                if (at == 0) {
                    all.insert(all.begin(), '1');
                }
            }
            digits.whole = all.substr(0, all.size() - places);
            digits.fraction = all.substr(all.size() - places);
        }
    }
    if (pad) {
        digits.fraction.resize(places, '0');
    } else {
        while (!digits.fraction.empty() && digits.fraction.back() == '0') {
            digits.fraction.pop_back();
        }
    }
    return textOf(digits);
}

/// @brief The exact digits of a `Rational` whose value has at most @p places fraction digits.
/// @param value  The value; the caller rounded it to @p places.
/// @param places The fraction digits written.
/// @return The canonical text, every one of @p places written.
[[nodiscard]] inline std::string exactText(math::Rational const& value, std::uint32_t places) {
    auto const den = static_cast<std::uint64_t>(value.denominator);
    std::uint64_t const magnitude = math::detail::absU64(value.numerator);
    Digits out{.negative = value.numerator < 0, .whole = std::to_string(magnitude / den), .fraction = {}};
    std::uint64_t remainder = magnitude % den;
    for (std::uint32_t at = 0; at < places; ++at) {
        // The next digit of remainder * 10 / den, without forming remainder * 10, which may not fit in 64 bits.
        std::uint64_t accumulated = 0;
        std::uint64_t digit = 0;
        for (int step = 0; step < 10; ++step) {
            accumulated += remainder;
            while (accumulated >= den) {
                accumulated -= den;
                ++digit;
            }
        }
        remainder = accumulated;
        out.fraction.push_back(static_cast<char>('0' + digit));
    }
    return textOf(out);
}

/// @brief The rounding mode of the arithmetic for a column's rule.
/// @param rounding The column's rule.
/// @return The mode.
[[nodiscard]] constexpr math::RoundingMode modeOf(table::Rounding rounding) noexcept {
    return rounding == table::Rounding::HalfUp ? math::RoundingMode::HalfAwayFromZero : math::RoundingMode::HalfEven;
}

/// @brief A `double`'s shortest round-trip text in fixed notation.
/// @param value The number.
/// @return The text; nothing for a value that is not finite.
[[nodiscard]] inline std::optional<std::string> doubleText(double value) {
    if (!std::isfinite(value)) {
        return std::nullopt;
    }
    // The longest fixed text of a finite double: 309 integer digits, a sign, a point and 767 fraction digits.
    std::array<char, 1100> buffer{};
    auto const written = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, std::chars_format::fixed);
    if (written.ec != std::errc{}) {
        return std::nullopt;
    }
    return std::string{buffer.data(), written.ptr};
}

/// @brief Whether @p kind's cells are numbers a column's `decimals` applies to.
/// @param kind The kind.
/// @return `true` for `Integer`, `Decimal`, `Quantity` and `Number`.
[[nodiscard]] constexpr bool numeric(table::ColumnKind kind) noexcept {
    using table::ColumnKind;
    return kind == ColumnKind::Integer || kind == ColumnKind::Decimal || kind == ColumnKind::Quantity ||
           kind == ColumnKind::Number;
}

/// @brief A text cell of a column whose kind is not text, read for the kind.
/// @param text The text.
/// @param kind The column's kind.
/// @return The value it reads as, the text itself for a text kind, or nothing for text that does not read.
[[nodiscard]] inline std::optional<table::Cell> readText(std::string const& text, table::ColumnKind kind) {
    using table::ColumnKind;
    switch (kind) {
        case ColumnKind::Integer:
        case ColumnKind::Key: {
            std::int64_t value = 0;
            auto const read = std::from_chars(text.data(), text.data() + text.size(), value);
            if (read.ec == std::errc{} && read.ptr == text.data() + text.size() && !text.empty()) {
                return table::Cell{value};
            }
            if (kind == ColumnKind::Key) {
                return table::Cell{text};
            }
            return std::nullopt;
        }
        case ColumnKind::Decimal:
        case ColumnKind::Quantity:
            if (auto parsed = table::parseDecimal(text)) {
                return table::Cell{*parsed};
            }
            return std::nullopt;
        case ColumnKind::Number: {
            if (auto parsed = table::parseDecimal(text)) {
                return table::Cell{*parsed};
            }
            // A double no exact decimal holds (1e+21, 1e-20, 9.3e+18) is its own text in a record; it is still a
            // number, shown from its shortest fixed-notation text.
            double number = 0;
            auto const read = std::from_chars(text.data(), text.data() + text.size(), number);
            if (read.ec == std::errc{} && read.ptr == text.data() + text.size() && !text.empty()) {
                return table::Cell{number};
            }
            return std::nullopt;
        }
        case ColumnKind::Bool:
            if (text == "true" || text == "false") {
                return table::Cell{text == "true"};
            }
            return std::nullopt;
        default:
            return table::Cell{text};
    }
}

/// @brief A cell as its column reads it: a text cell of a non-text column read for the kind.
/// @param cell The cell.
/// @param kind The column's kind.
/// @return The cell to show; nothing when its text does not read.
[[nodiscard]] inline std::optional<table::Cell> normalized(table::Cell const& cell, table::ColumnKind kind) {
    if (auto const* const text = std::get_if<std::string>(&cell)) {
        return readText(*text, kind);
    }
    return cell;
}

/// @brief A number cell's value as canonical text at the column's display, before the locale writes it.
/// @param cell    The cell: an `int64`, a `Rational`, a `double` or a `QuantityCell`.
/// @param display The column's display.
/// @return The canonical text; nothing when the value cannot be shown (an overflow, a non-finite number).
[[nodiscard]] inline std::optional<std::string> numberText(table::Cell const& cell,
                                                           table::ColumnDisplay const& display) {
    auto const ofRational = [&display](math::Rational const& value) -> std::optional<std::string> {
        std::uint32_t const places =
            std::min(display.decimals.value_or(value.decimalPlaces.value), math::kMaxDecimalPlaces);
        math::RoundingMode const mode =
            display.decimals ? modeOf(display.rounding) : math::RoundingMode::HalfAwayFromZero;
        auto const rounded = math::checkedRoundToDecimalPlaces(value, math::DecimalPlaces{places}, mode);
        if (!rounded) {
            return std::nullopt;
        }
        std::string const text = exactText(*rounded, places);
        if (!display.decimals || display.padPlaces) {
            return text;
        }
        return roundText(*digitsOf(text), places, display.rounding, false);
    };
    auto const ofText = [&display](std::string const& text) -> std::optional<std::string> {
        auto const digits = digitsOf(text);
        if (!digits) {
            return std::nullopt;
        }
        if (!display.decimals) {
            return textOf(*digits);
        }
        return roundText(*digits, *display.decimals, display.rounding, display.padPlaces);
    };
    if (auto const* const integer = std::get_if<std::int64_t>(&cell)) {
        return ofText(std::to_string(*integer));
    }
    if (auto const* const rational = std::get_if<math::Rational>(&cell)) {
        return ofRational(*rational);
    }
    if (auto const* const number = std::get_if<double>(&cell)) {
        auto const text = doubleText(*number);
        return text ? ofText(*text) : std::nullopt;
    }
    if (auto const* const quantity = std::get_if<table::QuantityCell>(&cell)) {
        auto const canonical = math::checkedMul(quantity->amount, quantity->toCanonical);
        return canonical ? ofRational(*canonical) : std::nullopt;
    }
    return std::nullopt;
}

/// @brief A measure in the unit it is shown in, as canonical text and the unit's symbol.
/// @param measure The cell.
/// @param units   The table's units.
/// @return The number and the symbol; nothing for an unknown unit, no registry, or a conversion that overflows.
[[nodiscard]] inline std::optional<std::pair<std::string, std::string>> measureText(table::Measure const& measure,
                                                                                    DisplayUnits const& units) {
    if (units.units == nullptr || !units.units->contains(measure.unit)) {
        return std::nullopt;
    }
    table::UnitInfo const& own = units.units->info(measure.unit);
    table::UnitInfo const* shown = &own;
    math::Rational amount = measure.amount;
    if (auto const chosen = units.byDimension.find(own.dimension); chosen != units.byDimension.end()) {
        if (auto const target = units.units->find(chosen->second); target && *target != measure.unit) {
            table::UnitInfo const& to = units.units->info(*target);
            if (to.dimension == own.dimension) {
                auto const base = math::checkedMul(measure.amount, own.toBase);
                auto const converted = base ? math::checkedDiv(*base, to.toBase) : base;
                if (!converted) {
                    return std::nullopt;
                }
                amount = *converted;
                shown = &to;
            }
        }
    }
    std::uint32_t const places = std::min(shown->decimals, math::kMaxDecimalPlaces);
    auto const rounded = math::checkedRoundToDecimalPlaces(amount, math::DecimalPlaces{places},
                                                           math::RoundingMode::HalfEven);
    if (!rounded) {
        return std::nullopt;
    }
    return std::pair{exactText(*rounded, places), shown->symbol};
}

/// @brief Days since 1970-01-01, or UTC seconds shifted to the display zone, as text in the column's pattern.
/// @param value   The days or the seconds.
/// @param column  The column: its kind and its pattern.
/// @param locale  The session's formatting: its patterns and the display zone.
/// @return The text; the number's own digits outside the years 0 to 9999.
[[nodiscard]] inline std::string dateText(std::int64_t value, table::TableColumn const& column,
                                          FormatContext const& locale) {
    bool const withTime = column.info.kind == table::ColumnKind::DateTime;
    // Days of 0000-01-01 and of 9999-12-31 from the epoch: the range a four-digit year spells.
    constexpr std::int64_t kFirstDay = -719'528;
    constexpr std::int64_t kLastDay = 2'932'896;
    constexpr std::int64_t kDaySeconds = 86'400;
    std::int64_t seconds = 0;
    if (!withTime) {
        if (value < kFirstDay || value > kLastDay) {
            return std::to_string(value);
        }
        seconds = value * kDaySeconds;
    } else {
        // A zone is less than a day from UTC: a value a day past either end is out of range in every zone, and the
        // shift below cannot overflow.
        if (value < (kFirstDay - 1) * kDaySeconds || value >= (kLastDay + 2) * kDaySeconds) {
            return std::to_string(value);
        }
        seconds = value + std::int64_t{locale.utcOffsetMinutes} * 60;
        if (seconds < kFirstDay * kDaySeconds || seconds >= (kLastDay + 1) * kDaySeconds) {
            return std::to_string(value);
        }
    }
    time::DateTime const local{std::chrono::sys_time<std::chrono::milliseconds>{std::chrono::seconds{seconds}}};
    std::string const& sessions = withTime ? locale.dateTimePattern : locale.datePattern;
    std::string_view const pattern = !column.display.date.empty() ? std::string_view{column.display.date}
                                                                  : std::string_view{sessions};
    if (!pattern.empty()) {
        if (std::string text = formatDate(pattern, local, locale.utcOffsetMinutes); !text.empty()) {
            return text;
        }
    }
    return formatDate(withTime ? "yyyy-MM-dd HH:mm" : "yyyy-MM-dd", local, locale.utcOffsetMinutes);
}

/// @brief Whether a label's operand names @p operand: by value for a `Decimal` or `Number` column, so `"3"` names
///        3.0; by text otherwise.
/// @param label   The label's operand.
/// @param operand The cell's operand.
/// @param kind    The column's kind.
/// @return `true` when they name the same value.
[[nodiscard]] inline bool sameOperand(std::string_view label, std::string_view operand, table::ColumnKind kind) {
    if (kind == table::ColumnKind::Decimal || kind == table::ColumnKind::Number) {
        auto const lhs = table::parseDecimal(label);
        auto const rhs = table::parseDecimal(operand);
        if (lhs && rhs) {
            return *lhs == *rhs;
        }
    }
    return label == operand;
}

}  // namespace detail::cells

/// @brief How a cell draws beside its text: empty, unloaded, invalid, or a text.
/// @param cell   The cell.
/// @param column Its column.
/// @param units  What a `Measure` cell is shown through.
/// @return `Unloaded` for an unloaded cell, `Empty` for an empty one, `Invalid` for text a non-text column cannot
///         read, a `Measure` with no registry or an unknown unit, and a number that cannot be shown; else `Text`.
[[nodiscard]] inline CellLook cellLook(table::Cell const& cell, table::TableColumn const& column,
                                       DisplayUnits const& units = {}) {
    if (std::holds_alternative<table::Unloaded>(cell)) {
        return CellLook::Unloaded;
    }
    if (std::holds_alternative<std::monostate>(cell)) {
        return CellLook::Empty;
    }
    auto const read = detail::cells::normalized(cell, column.info.kind);
    if (!read) {
        return CellLook::Invalid;
    }
    if (auto const* const measure = std::get_if<table::Measure>(&*read)) {
        return detail::cells::measureText(*measure, units) ? CellLook::Text : CellLook::Invalid;
    }
    bool const number = std::holds_alternative<std::int64_t>(*read) || std::holds_alternative<math::Rational>(*read) ||
                        std::holds_alternative<double>(*read) || std::holds_alternative<table::QuantityCell>(*read);
    bool const dated =
        column.info.kind == table::ColumnKind::Date || column.info.kind == table::ColumnKind::DateTime;
    if (number && !dated && !detail::cells::numberText(*read, column.display)) {
        return CellLook::Invalid;
    }
    return CellLook::Text;
}

/// @brief A cell's value as a filter entry takes it.
/// @param cell   The cell.
/// @param column Its column.
/// @param units  What a `Measure` cell's unit is named through.
/// @return An integer's digits, a `Rational` at its own places, a `double`'s shortest round-trip text, `"true"` or
///         `"false"`, text as it is, a quantity's canonical amount, a measure's amount, a space and its unit's id;
///         nothing for an empty, an unloaded or an invalid cell.
[[nodiscard]] inline std::optional<std::string> operandOf(table::Cell const& cell, table::TableColumn const& column,
                                                          DisplayUnits const& units = {}) {
    if (CellLook const look = cellLook(cell, column, units); look != CellLook::Text) {
        return std::nullopt;
    }
    table::Cell const read = *detail::cells::normalized(cell, column.info.kind);
    return std::visit(
        [&]<typename T>(T const& value) -> std::optional<std::string> {
            if constexpr (std::is_same_v<T, std::monostate> || std::is_same_v<T, table::Unloaded>) {
                return std::nullopt;
            } else if constexpr (std::is_same_v<T, std::int64_t>) {
                return std::to_string(value);
            } else if constexpr (std::is_same_v<T, math::Rational>) {
                return units::detail::formatRationalDecimal(value);
            } else if constexpr (std::is_same_v<T, double>) {
                return detail::cells::doubleText(value);
            } else if constexpr (std::is_same_v<T, bool>) {
                return std::string{value ? "true" : "false"};
            } else if constexpr (std::is_same_v<T, std::string>) {
                return value;
            } else if constexpr (std::is_same_v<T, table::QuantityCell>) {
                auto const canonical = math::checkedMul(value.amount, value.toCanonical);
                return canonical ? std::optional{units::detail::formatRationalDecimal(*canonical)} : std::nullopt;
            } else {
                static_assert(std::is_same_v<T, table::Measure>);
                return units::detail::formatRationalDecimal(value.amount) + " " + units.units->info(value.unit).id;
            }
        },
        read);
}

/// @brief The cell an operand names in @p column: `operandOf`'s inverse for the column's kind.
/// @param operand The operand.
/// @param column  The column.
/// @param units   What a `Measure` operand's unit id is found in.
/// @return The cell; the operand as text when it does not read as the kind, which shows as `Invalid`.
[[nodiscard]] inline table::Cell cellOfOperand(std::string_view operand, table::TableColumn const& column,
                                               DisplayUnits const& units = {}) {
    using table::ColumnKind;
    std::string const text{operand};
    switch (column.info.kind) {
        case ColumnKind::Integer:
        case ColumnKind::Key:
        case ColumnKind::Date:
        case ColumnKind::DateTime: {
            std::int64_t value = 0;
            auto const read = std::from_chars(text.data(), text.data() + text.size(), value);
            if (read.ec == std::errc{} && read.ptr == text.data() + text.size() && !text.empty()) {
                return table::Cell{value};
            }
            return table::Cell{text};
        }
        case ColumnKind::Decimal:
        case ColumnKind::Quantity:
            if (auto parsed = table::parseDecimal(text)) {
                return table::Cell{*parsed};
            }
            return table::Cell{text};
        case ColumnKind::Number: {
            double value = 0;
            auto const read = std::from_chars(text.data(), text.data() + text.size(), value);
            if (read.ec == std::errc{} && read.ptr == text.data() + text.size() && !text.empty()) {
                return table::Cell{value};
            }
            return table::Cell{text};
        }
        case ColumnKind::Bool:
            if (text == "true" || text == "false") {
                return table::Cell{text == "true"};
            }
            return table::Cell{text};
        case ColumnKind::Measure: {
            auto const space = text.find(' ');
            auto const amount = table::parseDecimal(std::string_view{text}.substr(0, space));
            std::string_view const id =
                space == std::string::npos ? std::string_view{} : std::string_view{text}.substr(space + 1);
            if (!amount) {
                return table::Cell{text};
            }
            std::optional<table::UnitId> const unit = units.units != nullptr ? units.units->find(id) : std::nullopt;
            return table::Cell{table::Measure{.amount = *amount, .unit = unit.value_or(table::kUnknownUnit)}};
        }
        default:
            return table::Cell{text};
    }
}

/// @brief A cell's text by its column's display, in the session's formatting.
///
/// An unloaded cell is `""`: the backend draws its placeholder. An empty cell is the column's `empty`. Text in a
/// numeric or `Bool` column is read for the kind; text that does not read is `""` (`CellLook::Invalid`), and text
/// that reads shows as the value it reads as. Then the first label whose operand names the cell's value gives the
/// text. Otherwise a number is rounded to `decimals` (or shown at its own places) and written with the locale's
/// separators, grouped when `grouping` says; a date in the column's pattern, else the session's, else ISO; a measure
/// in its unit (or the dimension's chosen one) to that unit's decimals, then a space and the unit's symbol.
/// @param cell   The cell.
/// @param column Its column.
/// @param locale The session's formatting: numbers, date patterns and the display zone.
/// @param units  What a `Measure` cell is shown through.
/// @return The text.
[[nodiscard]] inline std::string formatCell(table::Cell const& cell, table::TableColumn const& column,
                                            FormatContext const& locale, DisplayUnits const& units = {}) {
    CellLook const look = cellLook(cell, column, units);
    if (look == CellLook::Unloaded || look == CellLook::Invalid) {
        return {};
    }
    if (look == CellLook::Empty) {
        return column.display.empty;
    }
    table::Cell const read = *detail::cells::normalized(cell, column.info.kind);
    if (!column.display.labels.empty()) {
        if (std::optional<std::string> const operand = operandOf(read, column, units)) {
            for (table::ValueLabel const& label : column.display.labels) {
                if (detail::cells::sameOperand(label.operand, *operand, column.info.kind)) {
                    return label.text;
                }
            }
        }
    }
    render::NumericLocale const numbers =
        column.display.grouping ? locale.numeric : withoutGrouping(locale.numeric);
    bool const dated = column.info.kind == table::ColumnKind::Date || column.info.kind == table::ColumnKind::DateTime;
    if (auto const* const days = std::get_if<std::int64_t>(&read); days != nullptr && dated) {
        return detail::cells::dateText(*days, column, locale);
    }
    if (auto const* const measure = std::get_if<table::Measure>(&read)) {
        auto const shown = detail::cells::measureText(*measure, units);
        std::string out = render::formatCanonicalNumber(shown->first, numbers);
        if (!shown->second.empty()) {
            out += ' ';
            out += shown->second;
        }
        return out;
    }
    if (auto const text = detail::cells::numberText(read, column.display)) {
        return render::formatCanonicalNumber(*text, numbers);
    }
    return table::displayText(read);
}

/// @brief The values list's entries as the column shows them.
///
/// Each operand is read with `cellOfOperand` and formatted; a date bucket's range operand (`a..b`) keeps the server's
/// text. Entries whose texts are equal are formatted again at their own places (`decimals` cleared), so two values
/// that show alike stay two entries a user can tell apart.
/// @param values The distinct values, in the reply's order.
/// @param column Their column.
/// @param locale The session's formatting.
/// @param units  What a `Measure` value is shown through.
/// @return One text per value, in order.
[[nodiscard]] inline std::vector<std::string> distinctTexts(std::span<table::DistinctValue const> values,
                                                            table::TableColumn const& column,
                                                            FormatContext const& locale,
                                                            DisplayUnits const& units = {}) {
    std::vector<std::string> out;
    out.reserve(values.size());
    for (table::DistinctValue const& value : values) {
        // A date bucket's operand is a range (`a..b`), which no cell holds: the server's text names it.
        if (value.operand.contains("..")) {
            out.push_back(value.text);
            continue;
        }
        out.push_back(formatCell(cellOfOperand(value.operand, column, units), column, locale, units));
    }
    table::TableColumn ownPlaces = column;
    ownPlaces.display.decimals.reset();
    std::vector<std::string> const first = out;
    for (std::size_t at = 0; at < out.size(); ++at) {
        if (std::ranges::count(first, first.at(at)) > 1 && !values[at].operand.contains("..")) {
            out.at(at) = formatCell(cellOfOperand(values[at].operand, column, units), ownPlaces, locale, units);
        }
    }
    return out;
}

/// @brief A column header's text with its unit: what the header and "Copy with headers" write.
/// @param label The header's label.
/// @param unit  The column's unit; empty for none.
/// @return `"Mass [kg]"`, or the label alone.
[[nodiscard]] inline std::string headerText(std::string_view label, std::string_view unit) {
    if (unit.empty()) {
        return std::string{label};
    }
    return std::format("{} [{}]", label, unit);
}

/// @brief A column header's accessible name with its unit.
/// @param label The header's label.
/// @param unit  The column's unit; empty for none.
/// @return `"Mass, kg"`, or the label alone.
[[nodiscard]] inline std::string headerAccessibleName(std::string_view label, std::string_view unit) {
    if (unit.empty()) {
        return std::string{label};
    }
    return std::format("{}, {}", label, unit);
}

}  // namespace morph::ui

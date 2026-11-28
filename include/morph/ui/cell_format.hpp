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
#include <memory>
#include <morph/export.hpp>
#include <morph/table/description.hpp>
#include <morph/ui/expression.hpp>
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
[[nodiscard]] MORPH_API std::optional<Digits> digitsOf(std::string_view text);

/// @brief Canonical text of @p digits: no sign on a value whose digits are all zero.
/// @param digits The parts.
/// @return The text.
[[nodiscard]] MORPH_API std::string textOf(Digits const& digits);

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
// NOLINTNEXTLINE(readability-function-cognitive-complexity): the rounding decision reads the dropped tail, the half cases of each Rounding mode and the pad rule in sequence, each step needing the digits the one before left
[[nodiscard]] MORPH_API std::string roundText(Digits digits, std::size_t places, table::Rounding rounding, bool pad);

/// @brief The exact digits of a `Rational` whose value has at most @p places fraction digits.
/// @param value  The value; the caller rounded it to @p places.
/// @param places The fraction digits written.
/// @return The canonical text, every one of @p places written.
[[nodiscard]] MORPH_API std::string exactText(math::Rational const& value, std::uint32_t places);

/// @brief The rounding mode of the arithmetic for a column's rule.
/// @param rounding The column's rule.
/// @return The mode.
[[nodiscard]] constexpr math::RoundingMode modeOf(table::Rounding rounding) noexcept {
    return rounding == table::Rounding::HalfUp ? math::RoundingMode::HalfAwayFromZero : math::RoundingMode::HalfEven;
}

/// @brief A `double`'s shortest round-trip text in fixed notation.
/// @param value The number.
/// @return The text; nothing for a value that is not finite.
[[nodiscard]] MORPH_API std::optional<std::string> doubleText(double value);

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
[[nodiscard]] MORPH_API std::optional<table::Cell> readText(std::string const& text, table::ColumnKind kind);

/// @brief A cell as its column reads it: a text cell of a non-text column read for the kind.
/// @param cell The cell.
/// @param kind The column's kind.
/// @return The cell to show; nothing when its text does not read.
[[nodiscard]] MORPH_API std::optional<table::Cell> normalized(table::Cell const& cell, table::ColumnKind kind);

/// @brief A number cell's value as canonical text at the column's display, before the locale writes it.
/// @param cell    The cell: an `int64`, a `Rational`, a `double` or a `QuantityCell`.
/// @param display The column's display.
/// @return The canonical text; nothing when the value cannot be shown (an overflow, a non-finite number).
[[nodiscard]] MORPH_API std::optional<std::string> numberText(table::Cell const& cell,
                                                              table::ColumnDisplay const& display);

/// @brief A measure in the unit it is shown in, as canonical text and the unit's symbol.
/// @param measure The cell.
/// @param units   The table's units.
/// @return The number and the symbol; nothing for an unknown unit, no registry, or a conversion that overflows.
[[nodiscard]] MORPH_API std::optional<std::pair<std::string, std::string>> measureText(table::Measure const& measure,
                                                                                       DisplayUnits const& units);

/// @brief Days since 1970-01-01, or UTC seconds shifted to the display zone, as text in the column's pattern.
/// @param value   The days or the seconds.
/// @param column  The column: its kind and its pattern.
/// @param locale  The session's formatting: its patterns and the display zone.
/// @return The text; the number's own digits outside the years 0 to 9999.
[[nodiscard]] MORPH_API std::string dateText(std::int64_t value, table::TableColumn const& column,
                                             FormatContext const& locale);

/// @brief Whether a label's operand names @p operand: by value for a `Decimal` or `Number` column, so `"3"` names
///        3.0; by text otherwise.
/// @param label   The label's operand.
/// @param operand The cell's operand.
/// @param kind    The column's kind.
/// @return `true` when they name the same value.
[[nodiscard]] MORPH_API bool sameOperand(std::string_view label, std::string_view operand, table::ColumnKind kind);

}  // namespace detail::cells

/// @brief How a cell draws beside its text: empty, unloaded, invalid, or a text.
/// @param cell   The cell.
/// @param column Its column.
/// @param units  What a `Measure` cell is shown through.
/// @return `Unloaded` for an unloaded cell, `Empty` for an empty one, `Invalid` for text a non-text column cannot
///         read, a `Measure` with no registry or an unknown unit, and a number that cannot be shown; else `Text`.
[[nodiscard]] MORPH_API CellLook cellLook(table::Cell const& cell, table::TableColumn const& column,
                                          DisplayUnits const& units = {});

/// @brief A cell's value as a filter entry takes it.
/// @param cell   The cell.
/// @param column Its column.
/// @param units  What a `Measure` cell's unit is named through.
/// @return An integer's digits, a `Rational` at its own places, a `double`'s shortest round-trip text, `"true"` or
///         `"false"`, text as it is, a quantity's canonical amount, a measure's amount, a space and its unit's id;
///         nothing for an empty, an unloaded or an invalid cell.
[[nodiscard]] MORPH_API std::optional<std::string> operandOf(table::Cell const& cell, table::TableColumn const& column,
                                                             DisplayUnits const& units = {});

/// @brief The cell an operand names in @p column: `operandOf`'s inverse for the column's kind.
/// @param operand The operand.
/// @param column  The column.
/// @param units   What a `Measure` operand's unit id is found in.
/// @return The cell; the operand as text when it does not read as the kind, which shows as `Invalid`.
[[nodiscard]] MORPH_API table::Cell cellOfOperand(std::string_view operand, table::TableColumn const& column,
                                                  DisplayUnits const& units = {});

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
[[nodiscard]] MORPH_API std::string formatCell(table::Cell const& cell, table::TableColumn const& column,
                                               FormatContext const& locale, DisplayUnits const& units = {});

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
[[nodiscard]] MORPH_API std::vector<std::string> distinctTexts(std::span<table::DistinctValue const> values,
                                                               table::TableColumn const& column,
                                                               FormatContext const& locale,
                                                               DisplayUnits const& units = {});

/// @brief A column header's text with its unit: what the header and "Copy with headers" write.
/// @param label The header's label.
/// @param unit  The column's unit; empty for none.
/// @return `"Mass [kg]"`, or the label alone.
[[nodiscard]] MORPH_API std::string headerText(std::string_view label, std::string_view unit);

/// @brief A column header's accessible name with its unit.
/// @param label The header's label.
/// @param unit  The column's unit; empty for none.
/// @return `"Mass, kg"`, or the label alone.
[[nodiscard]] MORPH_API std::string headerAccessibleName(std::string_view label, std::string_view unit);

}  // namespace morph::ui

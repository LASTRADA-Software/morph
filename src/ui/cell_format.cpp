// SPDX-License-Identifier: Apache-2.0

// The non-template definitions of <morph/ui/cell_format.hpp>.

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
#include <morph/render/locale_format.hpp>
#include <morph/table/data_source.hpp>
#include <morph/table/description.hpp>
#include <morph/table/query.hpp>
#include <morph/ui/cell_format.hpp>
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

namespace morph::ui::detail::cells {

std::optional<Digits> digitsOf(std::string_view text) {
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
    auto const digit = [](char glyph) { return glyph >= '0' && glyph <= '9'; };
    if (out.whole.empty() || !std::ranges::all_of(out.whole, digit) || !std::ranges::all_of(out.fraction, digit) ||
        (dot != std::string_view::npos && out.fraction.empty())) {
        return std::nullopt;
    }
    return out;
}

std::string textOf(Digits const& digits) {
    bool const zero = std::ranges::all_of(digits.whole, [](char glyph) { return glyph == '0'; }) &&
                      std::ranges::all_of(digits.fraction, [](char glyph) { return glyph == '0'; });
    std::string out = digits.negative && !zero ? "-" : "";
    out += digits.whole;
    if (!digits.fraction.empty()) {
        out += '.';
        out += digits.fraction;
    }
    return out;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): the rounding decision reads the dropped tail, the half cases of each Rounding mode and the pad rule in sequence, each step needing the digits the one before left
std::string roundText(Digits digits, std::size_t places, table::Rounding rounding, bool pad) {
    if (digits.fraction.size() > places) {
        std::string_view const dropped = std::string_view{digits.fraction}.substr(places);
        bool const restZero = std::ranges::all_of(dropped.substr(1), [](char glyph) { return glyph == '0'; });
        char const first = dropped.front();
        digits.fraction.resize(places);
        bool roundUp = false;
        if (first > '5' || (first == '5' && !restZero)) {
            roundUp = true;
        } else if (first == '5') {
            char const last = places > 0 ? digits.fraction.back() : digits.whole.back();
            roundUp = rounding == table::Rounding::HalfUp || ((last - '0') % 2) != 0;
        }
        if (roundUp) {
            std::string all = digits.whole + digits.fraction;
            std::size_t index = all.size();
            while (index > 0) {
                --index;
                if (all.at(index) == '9') {
                    all.at(index) = '0';
                } else {
                    ++all.at(index);
                    break;
                }
                if (index == 0) {
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

std::string exactText(math::Rational const& value, std::uint32_t places) {
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

std::optional<std::string> doubleText(double value) {
    if (!std::isfinite(value)) {
        return std::nullopt;
    }
    // The longest fixed text of a finite double: 309 integer digits, a sign, a point and 767 fraction digits.
    std::array<char, 1100> buffer{};
    auto const written = std::to_chars(buffer.data(), std::to_address(buffer.end()), value, std::chars_format::fixed);
    if (written.ec != std::errc{}) {
        return std::nullopt;
    }
    return std::string{buffer.data(), written.ptr};
}

std::optional<table::Cell> readText(std::string const& text, table::ColumnKind kind) {
    using table::ColumnKind;
    switch (kind) {
        case ColumnKind::Integer:
        case ColumnKind::Key: {
            std::int64_t value = 0;
            auto const read = std::from_chars(text.data(), std::to_address(text.end()), value);
            if (read.ec == std::errc{} && read.ptr == std::to_address(text.end()) && !text.empty()) {
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
            if (auto const number = table::detail::parseReal(text)) {
                return table::Cell{*number};
            }
            return std::nullopt;
        }
        case ColumnKind::Bool:
            if (text == "true" || text == "false") {
                return table::Cell{text == "true"};
            }
            return std::nullopt;
        case ColumnKind::Text:
        case ColumnKind::Date:
        case ColumnKind::DateTime:
        case ColumnKind::Custom:
        case ColumnKind::Measure:
        default:
            return table::Cell{text};
    }
}

std::optional<table::Cell> normalized(table::Cell const& cell, table::ColumnKind kind) {
    if (auto const* const text = std::get_if<std::string>(&cell)) {
        return readText(*text, kind);
    }
    return cell;
}

std::optional<std::string> numberText(table::Cell const& cell, table::ColumnDisplay const& display) {
    auto const ofRational = [&display](math::Rational const& value) -> std::optional<std::string> {
        std::uint32_t const places =
            std::min(display.decimals.value_or(value.decimalPlaces.value), math::kMaxDecimalPlaces);
        math::RoundingMode const mode =
            display.decimals ? modeOf(display.rounding) : math::RoundingMode::HalfAwayFromZero;
        auto const rounded = math::checkedRoundToDecimalPlaces(value, math::DecimalPlaces{places}, mode);
        if (!rounded) {
            return std::nullopt;
        }
        // NOLINTNEXTLINE(misc-const-correctness): returned by name, which moves it; a const would be copied.
        std::string text = exactText(*rounded, places);
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

std::optional<std::pair<std::string, std::string>> measureText(table::Measure const& measure,
                                                               DisplayUnits const& units) {
    if (units.units == nullptr || !units.units->contains(measure.unit)) {
        return std::nullopt;
    }
    table::UnitInfo const& own = units.units->info(measure.unit);
    table::UnitInfo const* shown = &own;
    math::Rational amount = measure.amount;
    if (auto const chosen = units.byDimension.find(own.dimension); chosen != units.byDimension.end()) {
        if (auto const target = units.units->find(chosen->second); target && *target != measure.unit) {
            table::UnitInfo const& toUnit = units.units->info(*target);
            if (toUnit.dimension == own.dimension) {
                auto const base = math::checkedMul(measure.amount, own.toBase);
                auto const converted = base ? math::checkedDiv(*base, toUnit.toBase) : base;
                if (!converted) {
                    return std::nullopt;
                }
                amount = *converted;
                shown = &toUnit;
            }
        }
    }
    std::uint32_t const places = std::min(shown->decimals, math::kMaxDecimalPlaces);
    auto const rounded =
        math::checkedRoundToDecimalPlaces(amount, math::DecimalPlaces{places}, math::RoundingMode::HalfEven);
    if (!rounded) {
        return std::nullopt;
    }
    return std::pair{exactText(*rounded, places), shown->symbol};
}

std::string dateText(std::int64_t value, table::TableColumn const& column, FormatContext const& locale) {
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
        seconds = value + (std::int64_t{locale.utcOffsetMinutes} * 60);
        if (seconds < kFirstDay * kDaySeconds || seconds >= (kLastDay + 1) * kDaySeconds) {
            return std::to_string(value);
        }
    }
    time::DateTime const local{std::chrono::sys_time<std::chrono::milliseconds>{std::chrono::seconds{seconds}}};
    std::string const& sessions = withTime ? locale.dateTimePattern : locale.datePattern;
    std::string_view const pattern =
        !column.display.date.empty() ? std::string_view{column.display.date} : std::string_view{sessions};
    if (!pattern.empty()) {
        if (std::string text = formatDate(pattern, local, locale.utcOffsetMinutes); !text.empty()) {
            return text;
        }
    }
    return formatDate(withTime ? "yyyy-MM-dd HH:mm" : "yyyy-MM-dd", local, locale.utcOffsetMinutes);
}

bool sameOperand(std::string_view label, std::string_view operand, table::ColumnKind kind) {
    if (kind == table::ColumnKind::Decimal || kind == table::ColumnKind::Number) {
        auto const lhs = table::parseDecimal(label);
        auto const rhs = table::parseDecimal(operand);
        if (lhs && rhs) {
            return *lhs == *rhs;
        }
    }
    return label == operand;
}

}  // namespace morph::ui::detail::cells

namespace morph::ui {

CellLook cellLook(table::Cell const& cell, table::TableColumn const& column, DisplayUnits const& units) {
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
    bool const dated = column.info.kind == table::ColumnKind::Date || column.info.kind == table::ColumnKind::DateTime;
    if (number && !dated && !detail::cells::numberText(*read, column.display)) {
        return CellLook::Invalid;
    }
    return CellLook::Text;
}

std::optional<std::string> operandOf(table::Cell const& cell, table::TableColumn const& column,
                                     DisplayUnits const& units) {
    if (CellLook const look = cellLook(cell, column, units); look != CellLook::Text) {
        return std::nullopt;
    }
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access): a cell with a look has a normalized value.
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

table::Cell cellOfOperand(std::string_view operand, table::TableColumn const& column, DisplayUnits const& units) {
    using table::ColumnKind;
    std::string const text{operand};
    switch (column.info.kind) {
        case ColumnKind::Integer:
        case ColumnKind::Key:
        case ColumnKind::Date:
        case ColumnKind::DateTime: {
            std::int64_t value = 0;
            auto const read = std::from_chars(text.data(), std::to_address(text.end()), value);
            if (read.ec == std::errc{} && read.ptr == std::to_address(text.end()) && !text.empty()) {
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
            if (auto const value = table::detail::parseReal(text)) {
                return table::Cell{*value};
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
            std::string_view const columnId =
                space == std::string::npos ? std::string_view{} : std::string_view{text}.substr(space + 1);
            if (!amount) {
                return table::Cell{text};
            }
            std::optional<table::UnitId> const unit =
                units.units != nullptr ? units.units->find(columnId) : std::nullopt;
            return table::Cell{table::Measure{.amount = *amount, .unit = unit.value_or(table::kUnknownUnit)}};
        }
        case ColumnKind::Text:
        case ColumnKind::Custom:
        default:
            return table::Cell{text};
    }
}

std::string formatCell(table::Cell const& cell, table::TableColumn const& column, FormatContext const& locale,
                       DisplayUnits const& units) {
    CellLook const look = cellLook(cell, column, units);
    if (look == CellLook::Unloaded || look == CellLook::Invalid) {
        return {};
    }
    if (look == CellLook::Empty) {
        return column.display.empty;
    }
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access): a cell with a look has a normalized value.
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
    render::NumericLocale const numbers = column.display.grouping ? locale.numeric : withoutGrouping(locale.numeric);
    bool const dated = column.info.kind == table::ColumnKind::Date || column.info.kind == table::ColumnKind::DateTime;
    if (auto const* const days = std::get_if<std::int64_t>(&read); days != nullptr && dated) {
        return detail::cells::dateText(*days, column, locale);
    }
    if (auto const* const measure = std::get_if<table::Measure>(&read)) {
        if (auto const shown = detail::cells::measureText(*measure, units)) {
            std::string out = render::formatCanonicalNumber(shown->first, numbers);
            if (!shown->second.empty()) {
                out += ' ';
                out += shown->second;
            }
            return out;
        }
    }
    if (auto const text = detail::cells::numberText(read, column.display)) {
        return render::formatCanonicalNumber(*text, numbers);
    }
    return table::displayText(read);
}

std::vector<std::string> distinctTexts(std::span<table::DistinctValue const> values, table::TableColumn const& column,
                                       FormatContext const& locale, DisplayUnits const& units) {
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
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): `out` has one text per value.
        if (std::ranges::count(first, first.at(at)) > 1 && !values[at].operand.contains("..")) {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): `out` has one text per value.
            out.at(at) = formatCell(cellOfOperand(values[at].operand, column, units), ownPlaces, locale, units);
        }
    }
    return out;
}

std::string headerText(std::string_view label, std::string_view unit) {
    if (unit.empty()) {
        return std::string{label};
    }
    return std::format("{} [{}]", label, unit);
}

std::string headerAccessibleName(std::string_view label, std::string_view unit) {
    if (unit.empty()) {
        return std::string{label};
    }
    return std::format("{}, {}", label, unit);
}

}  // namespace morph::ui

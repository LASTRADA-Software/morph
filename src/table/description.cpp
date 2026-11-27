// SPDX-License-Identifier: Apache-2.0

// The non-template definitions of <morph/table/description.hpp>.

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <charconv>
#include <cmath>
#include <core/async/ExecutorContext.hpp>
#include <core/async/ParkedWork.hpp>
#include <core/async/Task.hpp>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <functional>
#include <glaze/glaze.hpp>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <morph/attributes.hpp>
#include <morph/core/action_refusal.hpp>
#include <morph/core/effects.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/payload_schema.hpp>
#include <morph/core/records.hpp>
#include <morph/forms/forms.hpp>
#include <morph/session/session.hpp>
#include <morph/table/bands.hpp>
#include <morph/table/data_source.hpp>
#include <morph/table/description.hpp>
#include <morph/table/edits.hpp>
#include <morph/table/level_cache.hpp>
#include <morph/table/query.hpp>
#include <morph/table/row_selection.hpp>
#include <morph/table/runtime.hpp>
#include <morph/table/view_cache.hpp>
#include <morph/util/rational.hpp>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <typeindex>
#include <utility>
#include <variant>
#include <vector>

// NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access) -- cell, column and row indices are bounded by the page's and the snapshot's own counts, checked before they are used

namespace morph::table {

TableDescription describe(TableDescription description) {
    description.revision.clear();
    auto const json = glz::write_json(description).value_or(std::string{});
    description.revision = model::detail::hex64(model::detail::fnv1a64(json));
    return description;
}

std::vector<ColumnInfo> columnInfos(TableDescription const& description) {
    std::vector<ColumnInfo> out;
    out.reserve(description.columns.size());
    for (auto const& column : description.columns) {
        out.push_back(column.info);
    }
    return out;
}

std::expected<std::shared_ptr<UnitRegistry const>, TableError> registryOf(TableDescription const& description) {
    UnitRegistryBuilder builder;
    for (auto const& unit : description.units) {
        if (unit.id.empty()) {
            continue;
        }
        if (auto added = builder.add(unit); !added) {
            return std::unexpected(std::move(added.error()));
        }
    }
    return std::move(builder).build();
}

std::expected<void, TableError> validateDisplay(TableDescription const& description) {
    using Refused = std::unexpected<TableError>;
    auto const refuse = [](std::string column, std::string message) {
        return Refused{TableError{
            .code = TableErrorCode::SourceContract, .column = std::move(column), .message = std::move(message)}};
    };
    auto const isCompanion = [&description](std::string_view columnId) {
        auto const found = std::ranges::find(description.columns, columnId,
                                             [](TableColumn const& column) -> auto const& { return column.info.id; });
        return found != description.columns.end() && found->info.kind == ColumnKind::Text;
    };
    for (TableColumn const& column : description.columns) {
        if (column.info.kind == ColumnKind::Measure && (column.display.decimals || !column.display.unit.empty())) {
            return refuse(column.info.id,
                          "a measure column shows each cell in its own unit; it takes no `decimals` or `unit`");
        }
        for (std::string const* const named : {&column.tone, &column.tooltip}) {
            if (!named->empty() && !isCompanion(*named)) {
                return refuse(*named, "column `" + column.info.id + "` names `" + *named +
                                          "` as a companion; a companion is a `Text` column of the table");
            }
        }
    }
    for (std::string const* const named : {&description.rowTone, &description.rowLock}) {
        if (!named->empty() && !isCompanion(*named)) {
            return refuse(*named,
                          "the table names `" + *named + "` as a row's companion; a companion is a `Text` column");
        }
    }
    return {};
}

std::expected<void, TableError> checkBands(std::span<BandInfo const> bands, std::span<TableColumn const> columns) {
    std::vector<std::string> ids;
    ids.reserve(columns.size());
    for (TableColumn const& column : columns) {
        ids.push_back(column.info.id);
    }
    if (std::optional<std::string> refused = bandRefusal(bands, ids)) {
        return std::unexpected(
            TableError{.code = TableErrorCode::SourceContract, .column = {}, .message = std::move(*refused)});
    }
    return {};
}

std::vector<std::string> columnsWithCompanions(TableDescription const& description,
                                               std::span<std::string const> columns,
                                               CompanionOverrides const& overrides) {
    std::vector<std::string> out;
    auto const add = [&out](std::string const& columnId) {
        if (!columnId.empty() && !std::ranges::contains(out, columnId)) {
            out.push_back(columnId);
        }
    };
    for (std::string const& columnId : columns) {
        add(columnId);
        auto const found = std::ranges::find(description.columns, columnId,
                                             [](TableColumn const& column) -> auto const& { return column.info.id; });
        if (found == description.columns.end()) {
            continue;
        }
        if (!overrides.tone.contains(columnId)) {
            add(found->tone);
        }
        if (!overrides.tooltip.contains(columnId)) {
            add(found->tooltip);
        }
    }
    if (!overrides.rowTone) {
        add(description.rowTone);
    }
    add(description.rowLock);
    return out;
}

std::vector<std::string> companionColumns(TableDescription const& description) {
    std::set<std::string, std::less<>> names;
    for (TableColumn const& column : description.columns) {
        names.insert(column.tone);
        names.insert(column.tooltip);
    }
    names.insert(description.rowTone);
    names.insert(description.rowLock);
    names.erase(std::string{});
    return {names.begin(), names.end()};
}

}  // namespace morph::table

namespace morph::table::detail {

void appendInteger(std::string& out, std::int64_t value) {
    std::array<char, 24> buffer{};
    auto const result = std::to_chars(std::to_address(buffer.begin()), std::to_address(buffer.end()), value);
    out.append(buffer.data(), result.ptr);
}

void appendJsonString(std::string& out, std::string_view text) {
    static constexpr std::string_view kHex = "0123456789abcdef";
    out += '"';
    for (char const character : text) {
        switch (character) {
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
                if (static_cast<unsigned char>(character) < 0x20U) {
                    auto const code = static_cast<unsigned char>(character);
                    out += "\\u00";
                    out += kHex[code >> 4U];
                    out += kHex[code & 0xFU];
                } else {
                    out += character;
                }
                break;
        }
    }
    out += '"';
}

void appendRational(std::string& out, math::Rational const& value) {
    auto numerator = value.numerator;
    auto denominator = value.denominator;
    auto const scale = powerOfTen(std::min(value.decimalPlaces.value, math::kMaxDecimalPlaces));
    if (denominator > 0 && scale % denominator == 0) {
        auto const factor = scale / denominator;
        constexpr auto kMax = std::numeric_limits<std::int64_t>::max();
        constexpr auto kMin = std::numeric_limits<std::int64_t>::min();
        if (factor == 1 || (numerator <= kMax / factor && numerator >= kMin / factor)) {
            numerator *= factor;
            denominator = scale;
        }
    }
    out += '[';
    appendInteger(out, numerator);
    out += ',';
    appendInteger(out, denominator);
    out += ']';
}

void appendDouble(std::string& out, double value) {
    if (std::isnan(value)) {
        out += "\"nan\"";
        return;
    }
    if (std::isinf(value)) {
        out += value > 0 ? "\"inf\"" : "\"-inf\"";
        return;
    }
    auto const text = glz::write_json(value).value_or(std::string{"null"});
    out += text;
    if (text.find_first_of(".eE") == std::string::npos) {
        out += ".0";
    }
}

void appendCellJson(std::string& out, Cell const& cell, std::map<UnitId, std::string> const& units) {
    std::visit(
        [&out, &units]<typename T>(T const& value) {
            if constexpr (std::is_same_v<T, std::monostate> || std::is_same_v<T, Unloaded>) {
                out += "null";
            } else if constexpr (std::is_same_v<T, std::int64_t>) {
                appendInteger(out, value);
            } else if constexpr (std::is_same_v<T, double>) {
                appendDouble(out, value);
            } else if constexpr (std::is_same_v<T, std::string>) {
                appendJsonString(out, value);
            } else if constexpr (std::is_same_v<T, bool>) {
                out += value ? "true" : "false";
            } else if constexpr (std::is_same_v<T, math::Rational>) {
                appendRational(out, value);
            } else if constexpr (std::is_same_v<T, Measure>) {
                appendRational(out, value.amount);
                out.back() = ',';
                if (auto const found = units.find(value.unit); found != units.end()) {
                    appendJsonString(out, found->second);
                } else {
                    out += "null";
                }
                out += ']';
            } else {
                static_assert(std::is_same_v<T, QuantityCell>);
                if (value.toCanonical.numerator == 1 && value.toCanonical.denominator == 1) {
                    appendRational(out, value.amount);
                    return;
                }
                appendRational(out, value.amount);
                out.back() = ',';
                appendRational(out, value.toCanonical);
                out += ']';
            }
        },
        cell);
}

void CellCursor::skipSpace() noexcept {
    while (_at < _text.size() &&
           (_text[_at] == ' ' || _text[_at] == '\t' || _text[_at] == '\n' || _text[_at] == '\r')) {
        ++_at;
    }
}

bool CellCursor::eat(char character) noexcept {
    skipSpace();
    if (_at < _text.size() && _text[_at] == character) {
        ++_at;
        return true;
    }
    return false;
}

bool CellCursor::atEnd() noexcept {
    skipSpace();
    return _at == _text.size();
}

std::string_view CellCursor::rest() noexcept {
    skipSpace();
    return _text.substr(_at);
}

std::optional<std::int64_t> CellCursor::integer() noexcept {
    skipSpace();
    auto const token = numberToken(_text.substr(_at));
    if (token.empty() || token.find_first_of(".eE") != std::string_view::npos) {
        return std::nullopt;
    }
    std::int64_t value = 0;
    auto const [end, error] = std::from_chars(std::to_address(token.begin()), std::to_address(token.end()), value);
    if (error != std::errc{} || end != std::to_address(token.end())) {
        return std::nullopt;
    }
    _at += token.size();
    return value;
}

std::string_view CellCursor::numberToken(std::string_view text) noexcept {
    std::size_t position = 0;
    auto const digits = [&] {
        auto const from = position;
        while (position < text.size() && text[position] >= '0' && text[position] <= '9') {
            ++position;
        }
        return position > from;
    };
    if (position < text.size() && text[position] == '-') {
        ++position;
    }
    if (!digits()) {
        return {};
    }
    if (position < text.size() && text[position] == '.') {
        ++position;
        if (!digits()) {
            return {};
        }
    }
    if (position < text.size() && (text[position] == 'e' || text[position] == 'E')) {
        ++position;
        if (position < text.size() && (text[position] == '+' || text[position] == '-')) {
            ++position;
        }
        if (!digits()) {
            return {};
        }
    }
    return text.substr(0, position);
}

std::optional<math::Rational> readPair(CellCursor& cursor) {
    auto const numerator = cursor.integer();
    if (!numerator || !cursor.eat(',')) {
        return std::nullopt;
    }
    auto const denominator = cursor.integer();
    if (!denominator || *denominator <= 0) {
        return std::nullopt;
    }
    return math::Rational{math::Numerator{*numerator}, math::Denominator{*denominator},
                          math::DecimalPlaces{placesOf(*denominator)}};
}

std::optional<Cell> readExactCell(std::string_view text) {
    CellCursor cursor{text};
    if (!cursor.eat('[')) {
        return std::nullopt;
    }
    auto const amount = readPair(cursor);
    if (!amount) {
        return std::nullopt;
    }
    if (cursor.eat(']')) {
        return cursor.atEnd() ? std::optional<Cell>{Cell{*amount}} : std::nullopt;
    }
    if (!cursor.eat(',') || !cursor.eat('[')) {
        return std::nullopt;
    }
    auto const factor = readPair(cursor);
    if (!factor || !cursor.eat(']') || !cursor.eat(']') || !cursor.atEnd()) {
        return std::nullopt;
    }
    return Cell{QuantityCell{.amount = *amount, .toCanonical = *factor}};
}

std::optional<std::pair<math::Rational, std::optional<std::string>>> readMeasureCell(std::string_view text) {
    CellCursor cursor{text};
    if (!cursor.eat('[')) {
        return std::nullopt;
    }
    auto const amount = readPair(cursor);
    if (!amount || !cursor.eat(',')) {
        return std::nullopt;
    }
    auto unit = cursor.rest();
    auto const last = unit.find_last_not_of(" \t\n\r");
    if (last == std::string_view::npos || unit[last] != ']') {
        return std::nullopt;
    }
    unit = unit.substr(0, last);
    unit = unit.substr(0, unit.find_last_not_of(" \t\n\r") + 1);
    if (unit == "null") {
        return std::pair{*amount, std::optional<std::string>{}};
    }
    std::string unitId;
    if (unit.empty() || unit.front() != '"' || glz::read<glz::opts{.null_terminated = false}>(unitId, unit)) {
        return std::nullopt;
    }
    return std::pair{*amount, std::optional<std::string>{std::move(unitId)}};
}

std::optional<double> nonFinite(std::string_view text) noexcept {
    if (text == "nan") {
        return std::numeric_limits<double>::quiet_NaN();
    }
    if (text == "inf") {
        return std::numeric_limits<double>::infinity();
    }
    if (text == "-inf") {
        return -std::numeric_limits<double>::infinity();
    }
    return std::nullopt;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one sequential pass whose steps share its running state; splitting it would thread that state through parameters.
Cell cellFromJson(std::string_view json, std::optional<ColumnKind> kind) {
    auto const first = json.find_first_not_of(" \t\n\r");
    if (first == std::string_view::npos) {
        return Cell{std::string{json}};
    }
    auto const text = json.substr(first, json.find_last_not_of(" \t\n\r") - first + 1);
    if (text == "null") {
        return Cell{};
    }
    if (text == "true" || text == "false") {
        return Cell{text == "true"};
    }
    bool const number = kind == ColumnKind::Number;
    if (text.front() == '"') {
        std::string decoded;
        if (glz::read<glz::opts{.null_terminated = false}>(decoded, text)) {
            return Cell{std::string{text}};
        }
        if (number) {
            if (auto const value = nonFinite(decoded)) {
                return Cell{*value};
            }
        }
        return Cell{std::move(decoded)};
    }
    if (text.front() == '[') {
        if (auto cell = readExactCell(text)) {
            return std::move(*cell);
        }
        return Cell{std::string{text}};
    }
    if (CellCursor::numberToken(text).size() == text.size()) {
        if (!number && text.find_first_of(".eE") == std::string_view::npos) {
            std::int64_t value = 0;
            auto const [end, error] =
                std::from_chars(std::to_address(text.begin()), std::to_address(text.end()), value);
            if (error == std::errc{} && end == std::to_address(text.end())) {
                return Cell{value};
            }
        }
        double value = 0;
        if (!glz::read<glz::opts{.null_terminated = false}>(value, text) && std::isfinite(value)) {
            return Cell{value};
        }
    }
    return Cell{std::string{text}};
}

void readPageRows(GenericPage& page, std::vector<std::vector<glz::raw_json_view>> const& rows) {
    page.rows.clear();
    page.rows.reserve(rows.size());
    for (auto const& row : rows) {
        GenericRow cells;
        cells.reserve(row.size());
        for (auto const& cell : row) {
            cells.push_back(cellFromJson(cell.str, std::nullopt));
        }
        page.rows.push_back(std::move(cells));
    }
}

glz::raw_json writePageRows(GenericPage const& page) {
    glz::raw_json out;
    out.str += '[';
    for (std::size_t row = 0; row < page.rows.size(); ++row) {
        out.str += row == 0 ? "[" : ",[";
        auto const& cells = page.rows[row];
        for (std::size_t column = 0; column < cells.size(); ++column) {
            if (column > 0) {
                out.str += ',';
            }
            appendCellJson(out.str, cells[column], page.units);
        }
        out.str += ']';
    }
    out.str += ']';
    return out;
}

}  // namespace morph::table::detail

namespace morph::table {

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one sequential pass whose steps share its running state; splitting it would thread that state through parameters.
GenericPage writeGenericPage(PageRows const& page, DataSource const& source, std::span<std::string const> columns,
                             UnitRegistry const* units) {
    auto const snapshot = source.snapshot();
    auto const infos = source.columns();
    std::vector<std::optional<std::size_t>> indices;
    indices.reserve(columns.size());
    for (auto const& column : columns) {
        indices.push_back(detail::findColumn(infos, column));
    }
    GenericPage out;
    out.columns.assign(columns.begin(), columns.end());
    out.keys.reserve(page.rows.size());
    out.rows.reserve(page.rows.size());
    std::vector<std::string_view> unloaded;
    for (std::size_t at = 0; at < page.rows.size(); ++at) {
        auto const row = page.rows[at];
        out.keys.push_back(snapshot->rowId(row));
        GenericRow cells;
        cells.reserve(indices.size());
        unloaded.clear();
        for (auto const& index : indices) {
            if (!index) {
                cells.emplace_back();
                continue;
            }
            cells.push_back(snapshot->cell(row, *index));
            if (std::holds_alternative<Unloaded>(cells.back())) {
                unloaded.emplace_back(infos[*index].level);
            } else if (auto const* const measure = std::get_if<Measure>(&cells.back())) {
                // NOLINTNEXTLINE(cert-dcl03-c,misc-static-assert): units is known only at run time.
                assert(units != nullptr && "writeGenericPage: a measure is written with a unit registry");
                if (units != nullptr && units->contains(measure->unit)) {
                    out.units.try_emplace(measure->unit, units->info(measure->unit).id);
                }
            }
        }
        out.rows.push_back(std::move(cells));
        if (!unloaded.empty()) {
            out.unloadedRows.push_back(UnloadedRow{
                .index = static_cast<std::int64_t>(at), .levels = detail::sortedLevels(unloaded), .failed = {}});
        }
    }
    out.total = page.total;
    out.offset = page.offset;
    out.totalKind = page.totalKind;
    out.undecided = page.undecided;
    out.awaiting = page.awaiting;
    out.failed = page.failed;
    out.indices = page.indices;
    out.viewRevision = page.viewRevision;
    out.unmatched = page.unmatched;
    return out;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one sequential pass whose steps share its running state; splitting it would thread that state through parameters.
std::expected<GenericPage, TableError> readGenericPage(std::string_view json, TableDescription const& description,
                                                       UnitRegistry const* units) {
    auto const invalid = [](std::string message) {
        return std::unexpected(
            TableError{.code = TableErrorCode::InvalidValue, .column = {}, .message = std::move(message)});
    };
    GenericPage page;
    if (glz::read<glz::opts{.null_terminated = false, .error_on_unknown_keys = false}>(page, json)) {
        return invalid("not a generic page");
    }
    std::vector<ColumnKind> kinds;
    kinds.reserve(page.columns.size());
    for (auto const& columnId : page.columns) {
        auto const found = std::ranges::find(description.columns, columnId,
                                             [](TableColumn const& column) -> auto const& { return column.info.id; });
        if (found == description.columns.end()) {
            return std::unexpected(detail::unknownColumn(columnId));
        }
        kinds.push_back(found->info.kind);
    }
    if (page.keys.size() != page.rows.size()) {
        return invalid("a page of " + std::to_string(page.keys.size()) + " keys and " +
                       std::to_string(page.rows.size()) + " rows");
    }
    for (std::size_t row = 0; row < page.rows.size(); ++row) {
        if (page.rows[row].size() != kinds.size()) {
            return invalid("row " + std::to_string(row) + " has " + std::to_string(page.rows[row].size()) +
                           " cells under " + std::to_string(kinds.size()) + " columns");
        }
    }
    // A window looks a row's levels up by binary search and places a keyed row by its index, so both lists are
    // checked against the rows here rather than trusted by every reader after.
    auto const rowCount = static_cast<std::int64_t>(page.rows.size());
    std::int64_t previous = -1;
    for (auto const& unloaded : page.unloadedRows) {
        if (unloaded.index <= previous || unloaded.index >= rowCount) {
            return invalid("an unloaded row at " + std::to_string(unloaded.index) + " in a page of " +
                           std::to_string(rowCount) + " rows, or out of order");
        }
        previous = unloaded.index;
    }
    if (!page.indices.empty() && page.indices.size() != page.rows.size()) {
        return invalid("a page of " + std::to_string(page.indices.size()) + " indices and " +
                       std::to_string(page.rows.size()) + " rows");
    }
    if (std::ranges::any_of(page.indices, [](std::int64_t index) { return index < 0; })) {
        return invalid("a negative view row in indices");
    }
    // The rows were read with no kind: a measure's array is text until its column is known to be `Measure`.
    std::shared_ptr<UnitRegistry const> built;
    for (std::size_t column = 0; column < kinds.size(); ++column) {
        if (kinds[column] != ColumnKind::Measure) {
            continue;
        }
        if (units == nullptr) {
            auto registry = registryOf(description);
            if (!registry) {
                return std::unexpected(std::move(registry.error()));
            }
            built = std::move(*registry);
            units = built.get();
        }
        for (auto& cells : page.rows) {
            auto& cell = cells[column];
            auto const* const text = std::get_if<std::string>(&cell);
            auto const measure =
                text != nullptr && text->starts_with('[') ? detail::readMeasureCell(*text) : std::nullopt;
            if (!measure) {
                continue;
            }
            auto const unit = measure->second ? units->find(*measure->second) : std::nullopt;
            if (unit) {
                page.units.try_emplace(*unit, *measure->second);
            }
            cell = Measure{.amount = measure->first, .unit = unit.value_or(kUnknownUnit)};
        }
    }
    // Only a `Number` column reads differently besides, taking an integer token or a non-finite string as a double.
    for (std::size_t column = 0; column < kinds.size(); ++column) {
        if (kinds[column] != ColumnKind::Number) {
            continue;
        }
        for (auto& cells : page.rows) {
            auto& cell = cells[column];
            if (auto const* const integer = std::get_if<std::int64_t>(&cell)) {
                cell = static_cast<double>(*integer);
            } else if (auto const* const text = std::get_if<std::string>(&cell)) {
                if (auto const real = detail::nonFinite(*text)) {
                    cell = *real;
                }
            }
        }
    }
    return page;
}

}  // namespace morph::table

namespace morph::table::detail {

std::vector<std::string> GenericWindowCells::columns() const {
    std::vector<std::string> out;
    out.reserve(cells.size());
    for (auto const& [column, values] : cells) {
        out.push_back(column);
    }
    return out;
}

void GenericWindowCells::truncate(std::size_t count) {
    for (auto& [column, values] : cells) {
        values.resize(std::min(count, values.size()));
    }
}

}  // namespace morph::table::detail

namespace morph::table {

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one sequential pass whose steps share its running state; splitting it would thread that state through parameters.
bool PageWindow<GenericRow>::accept(PageFetch const& fetch, GenericPage page) {
    return acceptReply(
        fetch, replyOf(page),
        [this, &page](detail::GenericWindowCells& content, detail::WindowFetchKind kind, std::size_t take) {
            if (kind == detail::WindowFetchKind::Whole) {
                content.cells.clear();
            }
            if (kind == detail::WindowFetchKind::Rest) {
                std::erase_if(content.cells, [&page](auto const& entry) {
                    return std::ranges::find(page.columns, entry.first) == page.columns.end();
                });
            }
            // Each reply column's target, found once; the rows are then read in order, row by
            // row, so the reply's cells are visited where they lie.
            std::vector<std::vector<Cell>*> targets(page.columns.size(), nullptr);
            for (std::size_t column = 0; column < page.columns.size(); ++column) {
                auto const& columnId = page.columns[column];
                if (kind == detail::WindowFetchKind::Rest && !content.holds(columnId)) {
                    continue;
                }
                auto& values = content.cells[columnId];
                if (kind != detail::WindowFetchKind::Rest) {
                    values.clear();
                }
                values.reserve(values.size() + take);
                targets[column] = &values;
            }
            for (std::size_t row = 0; row < take; ++row) {
                auto& cells = page.rows[row];
                for (std::size_t column = 0; column < targets.size(); ++column) {
                    if (targets[column] != nullptr) {
                        targets[column]->push_back(column < cells.size() ? std::move(cells[column]) : Cell{});
                    }
                }
            }
            limitColumns(content);
        });
}

std::vector<std::int64_t> PageWindow<GenericRow>::patch(RowsFetch const& fetch, GenericPage reply) {
    // The columns a page holds that the reply lacks go once per page, not once per row.
    Core::Held const* trimmed = nullptr;
    return patchRows(
        // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the page's position, then the reply's row it is taken from.
        fetch, replyOf(reply), [&reply, &trimmed](Core::Held& page, std::size_t position, std::size_t from) {
            if (trimmed != &page) {
                std::erase_if(page.content.cells, [&reply](auto const& entry) {
                    return std::ranges::find(reply.columns, entry.first) == reply.columns.end();
                });
                trimmed = &page;
            }
            auto& cells = reply.rows[from];
            for (std::size_t column = 0; column < reply.columns.size() && column < cells.size(); ++column) {
                auto const found = page.content.cells.find(reply.columns[column]);
                if (found != page.content.cells.end() && position < found->second.size()) {
                    found->second[position] = std::move(cells[column]);
                }
            }
        });
}

RowId const* PageWindow<GenericRow>::key(std::int64_t index) const {
    auto const* const page = heldAt(index);
    return page == nullptr ? nullptr : &page->keys[static_cast<std::size_t>(index % pageSize())];
}

Cell const* PageWindow<GenericRow>::cell(std::int64_t index, std::string_view column) const {
    auto const* const page = heldAt(index);
    if (page == nullptr) {
        return nullptr;
    }
    auto const found = page->content.cells.find(column);
    auto const position = static_cast<std::size_t>(index % pageSize());
    return found == page->content.cells.end() || position >= found->second.size() ? nullptr : &found->second[position];
}

detail::WindowReply PageWindow<GenericRow>::replyOf(GenericPage const& page) {
    return detail::WindowReply{.keys = page.keys,
                               .rows = page.rows.size(),
                               .unloadedRows = page.unloadedRows,
                               .total = page.total,
                               .totalKind = page.totalKind,
                               .undecided = page.undecided,
                               .awaiting = page.awaiting,
                               .failed = page.failed,
                               .indices = page.indices};
}

void PageWindow<GenericRow>::limitColumns(detail::GenericWindowCells& content) const {
    if (content.cells.size() <= maxColumns()) {
        return;
    }
    std::vector<std::pair<std::uint64_t, std::string_view>> candidates;
    candidates.reserve(content.cells.size());
    for (auto const& [column, values] : content.cells) {
        if (!shownNow(column)) {
            candidates.emplace_back(namedAt(column), column);
        }
    }
    std::ranges::sort(candidates);
    auto excess = content.cells.size() - maxColumns();
    std::vector<std::string> dropped;
    for (auto const& candidate : candidates) {
        if (excess == 0) {
            break;
        }
        dropped.emplace_back(candidate.second);
        --excess;
    }
    for (auto const& column : dropped) {
        content.cells.erase(column);
    }
}

std::expected<TableDescription, TableError> servedDescription(ServedTable const& served) {
    if (served.limits.maxSortKeys == 0 || served.limits.maxSortKeys > kMaxSortKeysBound) {
        return std::unexpected(TableError{.code = TableErrorCode::SourceContract,
                                          .column = {},
                                          .message = "a table's `limits.maxSortKeys` is 1 to 32"});
    }
    TableDescription description = served.description;
    description.capabilities.maxSortKeys = static_cast<std::uint32_t>(served.limits.maxSortKeys);
    auto const measure = std::ranges::find(description.columns, ColumnKind::Measure,
                                           [](TableColumn const& column) { return column.info.kind; });
    if (measure != description.columns.end()) {
        if (served.services.units == nullptr) {
            return std::unexpected(TableError{.code = TableErrorCode::SourceContract,
                                              .column = measure->info.id,
                                              .message = "a table with measures needs a unit registry"});
        }
        if (description.units.empty()) {
            description.units = unitsFor(*served.services.units, columnInfos(description));
        }
    }
    return describe(std::move(description));
}

}  // namespace morph::table

namespace morph::table::detail {

MORPH_PROCESS_UNIQUE std::uint64_t PinnedSource::nextPin() noexcept {
    static std::atomic<std::uint64_t> pins{0};
    return pins.fetch_add(1) + 1;
}

std::shared_ptr<RowSnapshot const> ScopedSource::snapshot() const {
    auto base = _source->snapshot();
    std::scoped_lock const lock{_mutex};
    if (base != _base || _scoped == nullptr) {
        std::vector<std::size_t> rows;
        rows.reserve(std::min(_keys.size(), base->rowCount()));
        for (std::size_t row = 0; row < base->rowCount(); ++row) {
            if (_keys.contains(base->rowId(row))) {
                rows.push_back(row);
            }
        }
        _scoped = std::make_shared<ScopedSnapshot const>(base, std::move(rows));
        _base = std::move(base);
    }
    return _scoped;
}

std::optional<std::string> viewQueryOf(TableQuery const& validated) {
    auto sort = glz::write_json(validated.sort);
    auto filters = glz::write_json(validated.filters);
    if (!sort || !filters) {
        return std::nullopt;
    }
    return *sort + "\n" + *filters;
}

ServedTable pinned(ServedTable served) {
    if (served.source != nullptr) {
        auto const columns = served.source->columns();
        auto const groups = served.source->groups();
        served.source = std::make_shared<PinnedSource const>(
            served.source->snapshot(), std::vector<ColumnInfo>(columns.begin(), columns.end()),
            std::vector<ColumnGroupInfo>(groups.begin(), groups.end()));
    }
    return served;
}

std::string orderColumnOf(TableQuery const& query, std::span<ColumnInfo const> columns, std::string_view level,
                          Services const& services, std::span<ColumnGroupInfo const> groups) {
    for (auto const& key : query.sort) {
        if (auto const index = findColumn(columns, key.column); index && columns[*index].level == level) {
            return key.column;
        }
    }
    if (!query.filters.empty()) {
        if (auto const filter = compileFilterGuarded(query.filters, columns, services, groups)) {
            for (auto const column : filter->columnsUsed()) {
                if (columns[column].level == level) {
                    return columns[column].id;
                }
            }
        }
    }
    if (query.distinct) {
        auto const listed =
            query.distinct->group.empty()
                ? std::span<std::string const>{&query.distinct->column, 1}
                : detail::groupColumns(groups, query.distinct->group).value_or(std::span<std::string const>{});
        for (auto const& columnId : listed) {
            if (auto const index = findColumn(columns, columnId); index && columns[*index].level == level) {
                return columnId;
            }
        }
    }
    return {};
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one sequential pass whose steps share its running state; splitting it would thread that state through parameters.
std::expected<GenericPage, TableError> answerQuery(ServedTable const& served, TableQuery const& query,
                                                   AnswerContext const& context) {
    if (auto shown = validateDisplay(served.description); !shown) {
        return std::unexpected(std::move(shown.error()));
    }
    if (context.rowScoped && served.scope.empty()) {
        return std::unexpected(TableError{
            .code = TableErrorCode::SourceContract, .column = {}, .message = "a row-scoped table needs a scope"});
    }
    if (served.source == nullptr) {
        return std::unexpected(
            TableError{.code = TableErrorCode::Unavailable, .column = {}, .message = "the table serves no source"});
    }
    auto const described = servedDescription(served);
    if (!described) {
        return std::unexpected(described.error());
    }
    if (query.keys && !served.description.capabilities.keys) {
        return std::unexpected(TableError{
            .code = TableErrorCode::Unavailable, .column = {}, .message = "this table answers no keyed query"});
    }
    if (query.distinct && !served.description.capabilities.distinct) {
        return std::unexpected(TableError{
            .code = TableErrorCode::Unavailable, .column = {}, .message = "this table answers no distinct request"});
    }
    auto const infos = columnInfos(served.description);
    auto const groups = served.source->groups();
    if (auto error = detail::checkGroups(served.source->columns(), groups)) {
        return std::unexpected(std::move(*error));
    }
    auto checked = validate(query, infos, served.limits, served.mayRead, groups);
    if (!checked) {
        return std::unexpected(std::move(checked.error()));
    }
    auto const levelOf = [&served](std::string_view level) -> std::shared_ptr<LevelSource> {
        for (auto const& source : served.levels) {
            if (source != nullptr && source->level() == level) {
                return source;
            }
        }
        return nullptr;
    };
    auto const sourceColumns = served.source->columns();
    for (auto const& info : infos) {
        auto const level = info.level.empty() ? nullptr : levelOf(info.level);
        bool const known = info.level.empty() ? findColumn(sourceColumns, info.id).has_value()
                                              : level == nullptr || findColumn(level->columns(), info.id).has_value();
        if (!known) {
            return std::unexpected(TableError{.code = TableErrorCode::UnknownColumn,
                                              .column = info.id,
                                              .message = "the table's source has no column " + info.id});
        }
    }
    if (checked->distinct) {
        // The listed column's level is an order level: the values list reads it for every candidate row.
        auto const needed = levelsNeeded(*checked, infos, served.mayRead, served.services.forTask(), groups);
        std::vector<LevelInput> inputs;
        for (auto const& level : needed.order) {
            if (auto source = levelOf(level)) {
                auto levelSnapshot = source->snapshot();
                inputs.push_back(LevelInput{.source = std::move(source), .snapshot = std::move(levelSnapshot)});
            }
        }
        LeveledSource const leveled{served.source, infos, std::move(inputs)};
        auto values = distinctValues(
            leveled, *checked, *checked->distinct,
            ApplyOptions{.limits = served.limits, .services = served.services, .mayRead = served.mayRead});
        if (!values) {
            return std::unexpected(std::move(values.error()));
        }
        // Every row is a candidate: the order levels are wanted for every key of the snapshot. The set is kept in
        // the view cache, under a key no sort or filter writes, so that the requests over one snapshot want one
        // kept set, which a level holds once and each later request repeats, as a page's view does.
        auto const snapshot = leveled.snapshot();
        auto const everyRow = [&]() -> std::expected<ViewComputation, TableError> {
            ViewComputation out{.view = {}, .awaiting = {}, .failed = {}, .orderKeys = {}};
            auto all = std::make_shared<std::vector<RowId>>();
            all->reserve(snapshot->rowCount());
            out.view.rows.reserve(snapshot->rowCount());
            for (std::size_t row = 0; row < snapshot->rowCount(); ++row) {
                all->push_back(snapshot->rowId(row));
                out.view.rows.push_back(static_cast<std::uint32_t>(row));
            }
            out.orderKeys = std::move(all);
            return out;
        };
        std::shared_ptr<std::vector<RowId> const> keys;
        if (!needed.order.empty()) {
            if (context.views != nullptr && !context.record.empty()) {
                auto const* const pinned = dynamic_cast<PinnedSource const*>(served.source.get());
                auto found = context.views->find(
                    ViewKey{.record = std::string{context.record}, .scope = served.scope, .query = "every row"},
                    served.source->snapshot(), {}, everyRow, pinned != nullptr ? pinned->pinnedAt() : 0);
                if (!found) {
                    return std::unexpected(std::move(found.error()));
                }
                keys = (*found)->orderKeys;
            } else {
                keys = everyRow()->orderKeys;
            }
        }
        std::vector<std::shared_ptr<LevelSource>> leased;
        for (auto const& level : needed.order) {
            auto const source = levelOf(level);
            if (source == nullptr) {
                continue;
            }
            if (auto wanted = source->wantSet(keys, LevelUse::Order, query.retry); !wanted) {
                // The request is refused: the levels that accepted its order use hold nothing for it.
                for (auto const& accepted : leased) {
                    accepted->unwant(*keys);
                }
                auto error = std::move(wanted.error());
                error.code = TableErrorCode::Unavailable;
                error.column = orderColumnOf(*checked, infos, level, served.services.forTask(), groups);
                return std::unexpected(std::move(error));
            }
            leased.push_back(source);
        }
        GenericPage page;
        page.revision = described->revision;
        page.distinct = std::move(*values);
        return page;
    }
    std::vector<std::string> columns;
    if (checked->columns) {
        columns = *checked->columns;
    } else {
        columns.reserve(infos.size());
        for (auto const& info : infos) {
            if (!served.mayRead || served.mayRead(info.id)) {
                columns.push_back(info.id);
            }
        }
    }
    // Before any view is computed: `writeGenericPage` writes a measure only through a registry.
    for (auto const& column : columns) {
        if (auto const columnIndex = findColumn(infos, column)) {
            if (auto missing = unitsMissing(infos[*columnIndex], served.services)) {
                return std::unexpected(std::move(*missing));
            }
        }
    }
    auto const needed = levelsNeeded(*checked, infos, served.mayRead, served.services.forTask(), groups);
    std::vector<std::string> levels = needed.order;
    levels.insert(levels.end(), needed.display.begin(), needed.display.end());
    std::ranges::sort(levels);
    auto const [firstDuplicate, lastDuplicate] = std::ranges::unique(levels);
    levels.erase(firstDuplicate, lastDuplicate);
    std::vector<LevelInput> inputs;
    for (auto const& level : levels) {
        if (auto source = levelOf(level)) {
            auto levelSnapshot = source->snapshot();
            inputs.push_back(LevelInput{.source = std::move(source), .snapshot = std::move(levelSnapshot)});
        }
    }
    LeveledSource const leveled{served.source, infos, inputs};
    ApplyOptions const options{.limits = served.limits, .services = served.services, .mayRead = served.mayRead};
    // The keys of a view's rows and of its undecided rows: what its order levels are wanted for.
    auto const keysOf = [](ComputedView const& computed, RowSnapshot const& rows) {
        auto out = std::make_shared<std::vector<RowId>>();
        out->reserve(computed.rows.size() + computed.undecided.size());
        for (auto const row : computed.rows) {
            out->push_back(rows.rowId(row));
        }
        for (auto const row : computed.undecided) {
            out->push_back(rows.rowId(row));
        }
        return std::shared_ptr<std::vector<RowId> const>{std::move(out)};
    };
    auto const compute = [&]() -> std::expected<ViewComputation, TableError> {
        auto view = computeView(leveled, *checked, options);
        if (!view) {
            return std::unexpected(std::move(view.error()));
        }
        ViewComputation out{.view = std::move(*view), .awaiting = {}, .failed = {}, .orderKeys = {}};
        if (!needed.order.empty()) {
            out.orderKeys = keysOf(out.view, *leveled.snapshot());
        }
        for (auto const& level : needed.order) {
            bool unloaded = false;
            bool failed = false;
            auto const look = [&](std::uint32_t row) {
                auto const state = leveled.state(row, level);
                unloaded = unloaded || state == LevelRead::Unloaded;
                failed = failed || state == LevelRead::Failed;
                return unloaded && failed;
            };
            if (!std::ranges::any_of(out.view.rows, look)) {
                static_cast<void>(std::ranges::any_of(out.view.undecided, look));
            }
            if (unloaded) {
                out.awaiting.push_back(level);
            }
            if (failed) {
                out.failed.push_back(level);
            }
        }
        return out;
    };
    auto const snapshot = leveled.snapshot();
    std::shared_ptr<ViewEntry const> entry;
    auto const viewQuery = context.views != nullptr && !context.record.empty() ? viewQueryOf(*checked) : std::nullopt;
    if (viewQuery) {
        // The pinned source's snapshot, whose identity survives across requests, and the order levels' states.
        std::vector<std::pair<std::string, std::uint64_t>> generations;
        for (auto const& level : needed.order) {
            for (auto const& input : inputs) {
                if (input.source->level() == level) {
                    generations.emplace_back(level, input.snapshot->generation());
                }
            }
        }
        auto const* const pinned = dynamic_cast<PinnedSource const*>(served.source.get());
        auto found = context.views->find(
            ViewKey{.record = std::string{context.record}, .scope = served.scope, .query = *viewQuery},
            served.source->snapshot(), generations, compute, pinned != nullptr ? pinned->pinnedAt() : 0);
        if (!found) {
            return std::unexpected(std::move(found.error()));
        }
        entry = std::move(*found);
    } else {
        auto computed = compute();
        if (!computed) {
            return std::unexpected(std::move(computed.error()));
        }
        entry = std::make_shared<ViewEntry const>(
            served.source->snapshot(), std::vector<std::pair<std::string, std::uint64_t>>{}, std::move(*computed));
    }
    auto const* const view = &entry->view;
    // A cached entry keeps the keyed lookups for the requests after this one; a request's own view is scanned once.
    // The retained rows are merged into a copy for this request alone: the entry is every session's.
    bool const lookups = viewQuery && (checked->keys || !checked->retain.empty());
    auto paged = lookups ? viewPage(*view, leveled, *snapshot, *checked, options,
                                    checked->retain.empty() ? entry->viewRowOf() : std::span<std::uint32_t const>{},
                                    [&entry](RowId const& key) { return entry->sourceRowOf(key); })
                         : viewPage(*view, leveled, *snapshot, *checked, options);
    if (!paged) {
        return std::unexpected(std::move(paged.error()));
    }
    auto rows = std::move(*paged);
    rows.awaiting = entry->awaiting;
    rows.failed = entry->failed;
    auto page = writeGenericPage(rows, leveled, columns, served.services.units.get());
    page.unloadedRows.clear();
    for (std::size_t at = 0; at < rows.rows.size(); ++at) {
        UnloadedRow row{.index = static_cast<std::int64_t>(at), .levels = {}, .failed = {}};
        for (auto const& level : levels) {
            auto const state = leveled.state(rows.rows[at], level);
            if (state != LevelRead::Loaded) {
                row.levels.push_back(level);
            }
            if (state == LevelRead::Failed) {
                row.failed.push_back(level);
            }
        }
        if (!row.levels.empty()) {
            page.unloadedRows.push_back(std::move(row));
        }
    }
    // A level reached below has a source, so it is in `generations`; a cached entry is found only for the same
    // order levels, which its computation read, so it built the keys.
    auto const& orderKeys = entry->orderKeys;
    std::vector<std::shared_ptr<LevelSource>> leased;
    for (auto const& level : needed.order) {
        auto const source = levelOf(level);
        if (source == nullptr) {
            continue;
        }
        auto const& keys = *orderKeys;
        if (auto wanted = source->wantSet(orderKeys, LevelUse::Order, query.retry); !wanted) {
            // The query is refused: the levels that accepted its order use hold nothing for it.
            for (auto const& accepted : leased) {
                accepted->unwant(keys);
            }
            auto error = std::move(wanted.error());
            error.code = TableErrorCode::Unavailable;
            error.column = orderColumnOf(*checked, infos, level, served.services.forTask(), groups);
            return std::unexpected(std::move(error));
        }
        leased.push_back(source);
    }
    for (auto const& level : needed.display) {
        if (auto const source = levelOf(level); source != nullptr && !page.keys.empty()) {
            // NOLINTNEXTLINE(bugprone-unused-return-value): a display want is never refused: only an order use can exceed the cache's bound.
            static_cast<void>(source->want(page.keys, LevelUse::Display, query.retry));
        }
    }
    page.revision = described->revision;
    return page;
}

std::string errorCodeName(TableErrorCode code) {
    auto text = glz::write_json(code).value_or(std::string{});
    if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
        text = text.substr(1, text.size() - 2);
    }
    return text;
}

model::ActionRefusal refusalOf(TableError const& error) {
    return model::ActionRefusal{"table." + errorCodeName(error.code),
                                {model::RefusalArg{.name = "column", .value = error.column}},
                                error.message};
}

TableContext callerContext() {
    static session::Context const empty{};
    auto const* const current = session::current();
    return TableContext{.session = current != nullptr ? *current : empty};
}

void AnswerAwaiter::await_suspend(std::coroutine_handle<> awaiting) {
    auto const shared = _shared;
    shared->continuation = awaiting;
    shared->context = ::core::async::ResumeTarget::current();
    // Nothing of this awaiter is touched after the post: the work may resume the coroutine, and free the
    // awaiter with its frame, before `post` returns.
    auto* const work = _work;
    work->post([shared, served = std::move(_served), query = std::move(_query), context = _context] {
        try {
            shared->answer = answerQuery(served, query, context);
        } catch (...) {
            shared->error = std::current_exception();
        }
        if (shared->context) {
            shared->context.submit(::core::async::ParkedWork{.resume = shared->continuation, .abandon = {}});
        } else {
            shared->continuation.resume();
        }
    });
}

std::expected<GenericPage, TableError> AnswerAwaiter::await_resume() {
    if (_shared->error) {
        std::rethrow_exception(_shared->error);
    }
    return std::move(_shared->answer);
}

}  // namespace morph::table::detail

// NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)

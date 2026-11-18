// SPDX-License-Identifier: Apache-2.0

#include "table/wide_table_model.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <iterator>
#include <morph/core/payload_schema.hpp>
#include <mutex>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// windows.h first: psapi.h needs its types.
#include <psapi.h>
#elif defined(__linux__)
#include <fstream>
#include <unistd.h>
#endif

namespace bench {

namespace detail {

// One column's cells, in the arrays its role uses; the others stay empty.
struct ColumnData {
    WideRole role = WideRole::Integer;
    std::vector<std::int64_t> values;    // integers, dates, date-times, numerators at the role's fixed denominator
    std::vector<std::uint8_t> flags;     // a quantity's unit (1 = kilograms), a boolean
    std::string text;                    // every text of the column, one after another
    std::vector<std::uint32_t> offsets;  // row r's text is [offsets[r], offsets[r + 1])
    std::vector<bool> empty;             // only for a sparse column

    [[nodiscard]] std::size_t bytes() const noexcept {
        return (values.capacity() * sizeof(std::int64_t)) + flags.capacity() + text.capacity() +
               (offsets.capacity() * sizeof(std::uint32_t)) + (empty.capacity() / 8);
    }
};

}  // namespace detail

namespace {

using morph::math::DecimalPlaces;
using morph::math::Denominator;
using morph::math::Numerator;
using morph::math::Rational;
using morph::table::Cell;
using morph::table::ColumnInfo;
using morph::table::ColumnKind;
using morph::table::QuantityCell;
using detail::ColumnData;

constexpr std::int64_t kDecimalScale = 100;
constexpr std::int64_t kGramScale = 1000;
constexpr std::int64_t kKilogramScale = 1'000'000;

// Each entry is one code point, so a text of n entries is n code points long.
constexpr std::array<std::string_view, 32> kSyllables{
    "a", "e", "i", "o", "u", "b", "c", "d", "f", "g", "k", "l", "m", "n", "p", "r",
    "s", "t", "v", "z", "\xC3\x89", "\xC3\xA9", "\xC3\x84", "\xC3\xA4", "\xC3\x96", "\xC3\xB8", "\xC3\x9C",
    "\xC3\xA7", "\xC3\x9F", "Z", "K", "M"};

[[nodiscard]] constexpr std::uint64_t splitmix64(std::uint64_t z) noexcept {
    z += 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31U);
}

[[nodiscard]] std::uint64_t columnBase(std::uint64_t seed, std::string_view column) noexcept {
    return splitmix64(seed ^ morph::model::detail::fnv1a64(column));
}

[[nodiscard]] std::int64_t signedBelow(std::uint64_t bits, std::uint64_t modulus, std::int64_t offset) noexcept {
    return static_cast<std::int64_t>(bits % modulus) - offset;
}

[[nodiscard]] ColumnKind kindOf(WideRole role) noexcept {
    switch (role) {
        case WideRole::Decimal:
        case WideRole::Measure:
            return ColumnKind::Decimal;
        case WideRole::Quantity:
            return ColumnKind::Quantity;
        case WideRole::Text:
        case WideRole::LongText:
            return ColumnKind::Text;
        case WideRole::Date:
            return ColumnKind::Date;
        case WideRole::DateTime:
            return ColumnKind::DateTime;
        case WideRole::Bool:
            return ColumnKind::Bool;
        case WideRole::Key:
        case WideRole::Integer:
        default:
            return ColumnKind::Integer;
    }
}

[[nodiscard]] bool isText(WideRole role) noexcept { return role == WideRole::Text || role == WideRole::LongText; }

[[nodiscard]] bool usesFlags(WideRole role) noexcept { return role == WideRole::Quantity || role == WideRole::Bool; }

void appendText(std::string& out, std::uint64_t bits, WideRole role) {
    auto const length = role == WideRole::LongText ? 200 + (bits % 1801) : 4 + (bits % 57);
    for (std::uint64_t i = 0; i < length; ++i) {
        out += kSyllables.at(splitmix64(bits + i) % kSyllables.size());
    }
}

// Generates one column's arrays for rows [0, rows).
std::shared_ptr<ColumnData const> generate(std::uint64_t seed, WideColumn const& column, std::int64_t rows) {
    auto data = std::make_shared<ColumnData>();
    data->role = column.role;
    auto const count = static_cast<std::size_t>(rows);
    if (isText(column.role)) {
        data->offsets.reserve(count + 1);
        data->offsets.push_back(0);
    } else {
        data->values.reserve(column.role == WideRole::Bool ? 0 : count);
        data->flags.reserve(usesFlags(column.role) ? count : 0);
    }
    if (column.sparse) {
        data->empty.reserve(count);
    }
    auto const base = columnBase(seed, column.info.id);
    for (std::int64_t row = 0; row < rows; ++row) {
        auto const bits = splitmix64(base + static_cast<std::uint64_t>(row));
        bool const empty = column.sparse && bits % 10 == 0;
        if (column.sparse) {
            data->empty.push_back(empty);
        }
        switch (column.role) {
            case WideRole::Key:
                data->values.push_back(row + 1);
                break;
            case WideRole::Integer:
                data->values.push_back(empty ? 0 : signedBelow(bits, 2'000'001, 1'000'000));
                break;
            case WideRole::Decimal:
                data->values.push_back(empty ? 0 : signedBelow(bits, 20'000'001, 10'000'000));
                break;
            case WideRole::Quantity:
                data->values.push_back(empty ? 0 : static_cast<std::int64_t>(bits % 10'000'001));
                data->flags.push_back(empty ? 0 : static_cast<std::uint8_t>((bits >> 32U) & 1U));
                break;
            case WideRole::Measure:
                data->values.push_back(empty ? 0 : static_cast<std::int64_t>(bits % 1'000'001));
                break;
            case WideRole::Text:
            case WideRole::LongText:
                if (!empty) {
                    appendText(data->text, bits, column.role);
                }
                data->offsets.push_back(static_cast<std::uint32_t>(data->text.size()));
                break;
            case WideRole::Date:
                data->values.push_back(empty ? 0 : static_cast<std::int64_t>(bits % 20'000));
                break;
            case WideRole::DateTime:
                data->values.push_back(empty ? 0 : static_cast<std::int64_t>(bits % 1'700'000'000));
                break;
            case WideRole::Bool:
            default:
                data->flags.push_back(empty ? 0 : static_cast<std::uint8_t>(bits & 1U));
                break;
        }
    }
    return data;
}

[[nodiscard]] Cell cellOf(ColumnData const& data, std::size_t row) {
    if (!data.empty.empty() && data.empty[row]) {
        return Cell{};
    }
    switch (data.role) {
        case WideRole::Key:
        case WideRole::Integer:
        case WideRole::Date:
        case WideRole::DateTime:
            return Cell{data.values[row]};
        case WideRole::Decimal:
        case WideRole::Measure:
            return Cell{Rational{Numerator{data.values[row]}, Denominator{kDecimalScale}, DecimalPlaces{2}}};
        case WideRole::Quantity:
            if (data.flags[row] != 0) {
                return Cell{QuantityCell{
                    .amount = Rational{Numerator{data.values[row]}, Denominator{kKilogramScale}, DecimalPlaces{6}},
                    .toCanonical = Rational{Numerator{1000}, Denominator{1}, DecimalPlaces{0}}}};
            }
            return Cell{Rational{Numerator{data.values[row]}, Denominator{kGramScale}, DecimalPlaces{3}}};
        case WideRole::Text:
        case WideRole::LongText:
            return Cell{data.text.substr(data.offsets[row], data.offsets[row + 1] - data.offsets[row])};
        case WideRole::Bool:
        default:
            return Cell{data.flags[row] != 0};
    }
}

class WideSnapshot final : public morph::table::RowSnapshot {
public:
    WideSnapshot(std::int64_t rows, std::vector<std::shared_ptr<ColumnData const>> columns)
        : _rows{static_cast<std::size_t>(rows)}, _columns{std::move(columns)} {}

    [[nodiscard]] std::size_t rowCount() const override { return _rows; }
    [[nodiscard]] morph::table::RowId rowId(std::size_t row) const override {
        return static_cast<std::int64_t>(row) + 1;
    }
    [[nodiscard]] Cell cell(std::size_t row, std::size_t column) const override {
        return cellOf(*_columns.at(column), row);
    }
    void readColumn(std::size_t column, morph::table::ColumnSink& sink, std::size_t first,
                    std::size_t last) const override {
        constexpr std::size_t kRun = 256;
        ColumnData const& data = *_columns.at(column);
        std::array<Cell, kRun> buffer;
        for (std::size_t begin = first; begin < last; begin += kRun) {
            auto const end = std::min(last, begin + kRun);
            for (std::size_t row = begin; row < end; ++row) {
                buffer[row - begin] = cellOf(data, row);
            }
            sink.cells(begin, std::span<Cell const>{buffer.data(), end - begin});
        }
    }

private:
    std::size_t _rows;
    std::vector<std::shared_ptr<ColumnData const>> _columns;
};

[[nodiscard]] std::int64_t exactlyScaled(Rational const& value, std::int64_t scale) {
    if (scale % value.denominator != 0) {
        throw std::invalid_argument{"wide table: a value with more places than its column"};
    }
    return value.numerator * (scale / value.denominator);
}

// Writes `value` into row `row` of `data`; throws when it is not the column's alternative.
void store(ColumnData& data, std::size_t rows, std::size_t row, Cell const& value) {
    bool const empty = std::holds_alternative<std::monostate>(value);
    if (empty && data.role == WideRole::Key) {
        throw std::invalid_argument{"wide table: the key is never empty"};
    }
    if (empty && data.empty.empty()) {
        data.empty.assign(rows, false);
    }
    if (!data.empty.empty()) {
        data.empty[row] = empty;
    }
    auto const mismatch = [] { return std::invalid_argument{"wide table: a cell of another kind than its column"}; };
    switch (data.role) {
        case WideRole::Key:
        case WideRole::Integer:
        case WideRole::Date:
        case WideRole::DateTime:
            if (!empty && !std::holds_alternative<std::int64_t>(value)) {
                throw mismatch();
            }
            data.values[row] = empty ? 0 : std::get<std::int64_t>(value);
            return;
        case WideRole::Decimal:
        case WideRole::Measure:
            if (!empty && !std::holds_alternative<Rational>(value)) {
                throw mismatch();
            }
            data.values[row] = empty ? 0 : exactlyScaled(std::get<Rational>(value), kDecimalScale);
            return;
        case WideRole::Quantity:
            if (empty) {
                data.values[row] = 0;
                data.flags[row] = 0;
            } else if (auto const* const grams = std::get_if<Rational>(&value)) {
                data.values[row] = exactlyScaled(*grams, kGramScale);
                data.flags[row] = 0;
            } else if (auto const* const quantity = std::get_if<QuantityCell>(&value);
                       quantity != nullptr && quantity->toCanonical == Rational{1000, DecimalPlaces{0}}) {
                data.values[row] = exactlyScaled(quantity->amount, kKilogramScale);
                data.flags[row] = 1;
            } else {
                throw mismatch();
            }
            return;
        case WideRole::Text:
        case WideRole::LongText: {
            if (!empty && !std::holds_alternative<std::string>(value)) {
                throw mismatch();
            }
            std::string_view const replacement = empty ? std::string_view{} : std::get<std::string>(value);
            auto const begin = data.offsets[row];
            auto const oldLength = data.offsets[row + 1] - begin;
            data.text.replace(begin, oldLength, replacement);
            auto const delta = static_cast<std::int64_t>(replacement.size()) - static_cast<std::int64_t>(oldLength);
            for (std::size_t next = row + 1; next < data.offsets.size(); ++next) {
                data.offsets[next] = static_cast<std::uint32_t>(static_cast<std::int64_t>(data.offsets[next]) + delta);
            }
            return;
        }
        case WideRole::Bool:
        default:
            if (!empty && !std::holds_alternative<bool>(value)) {
                throw mismatch();
            }
            data.flags[row] = empty ? 0 : static_cast<std::uint8_t>(std::get<bool>(value));
            return;
    }
}

}  // namespace

std::uint64_t cellBits(std::uint64_t seed, std::string_view column, std::int64_t row) noexcept {
    return splitmix64(columnBase(seed, column) + static_cast<std::uint64_t>(row));
}

std::vector<WideColumn> wideColumns(int columns) {
    if (columns != 300 && columns != 30) {
        throw std::invalid_argument{std::format("wide table: {} columns; the layouts are 300 and 30", columns)};
    }
    std::vector<WideColumn> all;
    all.reserve(300);
    auto const add = [&](std::string id, WideRole role, std::string level = {}, std::string group = {}) {
        bool const sparse = all.size() % 12 == 11;
        all.push_back(WideColumn{
            .info =
                ColumnInfo{.id = std::move(id), .kind = kindOf(role), .comparator = {}, .format = {}, .level = level},
            .role = role,
            .sparse = sparse,
            .level = std::move(level),
            .group = std::move(group)});
    };
    add("id", WideRole::Key);
    for (int i = 1; i <= 99; ++i) {
        add(std::format("i{:03}", i), WideRole::Integer);
    }
    for (int i = 0; i < 60; ++i) {
        add(std::format("d{:03}", i), WideRole::Decimal, i < 10 ? "recipe" : "");
    }
    for (int i = 0; i < 30; ++i) {
        add(std::format("q{:03}", i), WideRole::Quantity);
    }
    for (int i = 0; i < 10; ++i) {
        add(std::format("m{:03}", i), WideRole::Measure);
    }
    add("t000", WideRole::LongText);
    for (int i = 1; i < 70; ++i) {
        std::string group;
        if (i >= 4 && i <= 6) {
            group = "g3";
        } else if (i >= 11 && i <= 26) {
            group = "g16";
        }
        add(std::format("t{:03}", i), WideRole::Text, i <= 10 ? "recipe" : "", std::move(group));
    }
    for (int i = 0; i < 10; ++i) {
        add(std::format("a{:03}", i), WideRole::Date);
    }
    for (int i = 0; i < 10; ++i) {
        add(std::format("s{:03}", i), WideRole::DateTime);
    }
    for (int i = 0; i < 10; ++i) {
        add(std::format("b{:03}", i), WideRole::Bool);
    }
    if (columns == 300) {
        return all;
    }
    // The small layout keeps the wide one's columns, attributes included, so a shared column has the same cells.
    static constexpr std::array<std::string_view, 30> kSmall{
        "id",   "i001", "i002", "i003", "i004", "i005", "i006", "i007", "i008", "i009",
        "d000", "d001", "d002", "d003", "d004", "d005", "q000", "q001", "q002", "m000",
        "t000", "t001", "t002", "t003", "t004", "t005", "t006", "a000", "s000", "b000"};
    std::vector<WideColumn> small;
    small.reserve(kSmall.size());
    for (auto const& column : all) {
        if (std::ranges::find(kSmall, column.info.id) != kSmall.end()) {
            small.push_back(column);
        }
    }
    return small;
}

namespace {

// The levels the columns of `description` name, sorted, each once, the base level left out.
std::vector<std::string> levelsOf(morph::table::TableDescription const& description) {
    std::vector<std::string> out;
    for (auto const& column : description.columns) {
        if (!column.info.level.empty()) {
            out.push_back(column.info.level);
        }
    }
    std::ranges::sort(out);
    auto const [first, last] = std::ranges::unique(out);
    out.erase(first, last);
    return out;
}

}  // namespace

morph::table::TableDescription wideDescription(std::string id, int columns) {
    morph::table::TableDescription out;
    out.label = id;
    out.id = std::move(id);
    out.key = "id";
    for (auto const& column : wideColumns(columns)) {
        out.columns.push_back(
            morph::table::TableColumn{.info = column.info, .label = column.info.id, .group = column.group});
        if (column.group.empty()) {
            continue;
        }
        auto group = std::ranges::find(out.groups, column.group, &morph::table::ColumnGroupInfo::id);
        if (group == out.groups.end()) {
            out.groups.push_back(
                morph::table::ColumnGroupInfo{.id = column.group, .label = column.group, .columns = {}});
            group = std::prev(out.groups.end());
        }
        group->columns.push_back(column.info.id);
    }
    out.levels = levelsOf(out);
    return out;
}

std::size_t approximateCellBytes(Cell const& cell) noexcept {
    std::size_t bytes = sizeof(Cell);
    if (auto const* const text = std::get_if<std::string>(&cell)) {
        if (text->capacity() > std::string{}.capacity()) {
            bytes += text->capacity() + 1;
        }
    }
    return bytes;
}

WideTableSource::WideTableSource(WideTableOptions options)
    : _options{options}, _layout{wideColumns(options.columns)} {
    if (options.rows < 0) {
        throw std::invalid_argument{"wide table: a negative row count"};
    }
    _columns.reserve(_layout.size());
    for (auto const& column : _layout) {
        _columns.push_back(column.info);
    }
    regenerate(options.seed);
}

std::span<ColumnInfo const> WideTableSource::columns() const { return _columns; }

std::shared_ptr<morph::table::RowSnapshot const> WideTableSource::snapshot() const {
    std::scoped_lock const lock{_mutex};
    return _snapshot;
}

void WideTableSource::subscribe(morph::table::ChangeListener& listener) { _listeners.add(listener); }

void WideTableSource::unsubscribe(morph::table::ChangeListener& listener) { _listeners.remove(listener); }

std::vector<WideColumn> const& WideTableSource::layout() const noexcept { return _layout; }

WideTableOptions WideTableSource::options() const {
    std::scoped_lock const lock{_mutex};
    return _options;
}

std::size_t WideTableSource::residentBytes() const noexcept {
    std::scoped_lock const lock{_mutex};
    std::size_t bytes = 0;
    for (auto const& data : _data) {
        bytes += data->bytes();
    }
    return bytes;
}

void WideTableSource::setCell(std::int64_t row, std::string_view column, Cell value) {
    auto const at = std::ranges::find(_columns, column, &ColumnInfo::id);
    if (at == _columns.end()) {
        throw std::invalid_argument{std::format("wide table: no column {}", column)};
    }
    if (row < 0 || row >= _options.rows) {
        throw std::invalid_argument{std::format("wide table: no row {}", row)};
    }
    auto const index = static_cast<std::size_t>(at - _columns.begin());
    {
        // Only writers change `_data`, and they hold `_writer`: the copy is read and built without `_mutex`.
        std::scoped_lock const writer{_writer};
        auto copy = std::make_shared<ColumnData>(*_data[index]);
        store(*copy, static_cast<std::size_t>(_options.rows), static_cast<std::size_t>(row), value);
        auto data = _data;
        data[index] = std::move(copy);
        auto snapshot = std::make_shared<WideSnapshot const>(_options.rows, data);
        std::scoped_lock const lock{_mutex};
        _data = std::move(data);
        _snapshot = std::move(snapshot);
    }
    _listeners.notify(morph::table::RowChange{.kind = morph::table::ChangeKind::Updated,
                                              .rows = {static_cast<std::size_t>(row)}});
}

void WideTableSource::regenerate(std::uint64_t seed) {
    std::unique_lock writer{_writer};
    std::vector<std::shared_ptr<ColumnData const>> data;
    data.reserve(_layout.size());
    for (auto const& column : _layout) {
        data.push_back(generate(seed, column, _options.rows));
    }
    auto snapshot = std::make_shared<WideSnapshot const>(_options.rows, data);
    {
        std::scoped_lock const lock{_mutex};
        _options.seed = seed;
        _data = std::move(data);
        _snapshot = std::move(snapshot);
    }
    writer.unlock();
    _listeners.notify(morph::table::RowChange{.kind = morph::table::ChangeKind::Reset, .rows = {}});
}

namespace {

// Whether row `row` starts loaded: its draw, uniform in [0, 1), is below the fraction.
[[nodiscard]] bool drawnLoaded(std::uint64_t seed, std::size_t row, double loadedFraction) noexcept {
    constexpr double kUnit = 1.0 / 9007199254740992.0;  // 2^-53
    auto const bits = cellBits(seed, "loaded", static_cast<std::int64_t>(row));
    return static_cast<double>(bits >> 11U) * kUnit < loadedFraction;
}

// Hands a slow column's run on with its unloaded rows replaced.
class MaskingSink final : public morph::table::ColumnSink {
public:
    MaskingSink(morph::table::ColumnSink& out, std::vector<bool> const& loaded) : _out{&out}, _loaded{&loaded} {}

    void cells(std::size_t firstRow, std::span<Cell const> cells) override {
        _run.assign(cells.begin(), cells.end());
        for (std::size_t i = 0; i < _run.size(); ++i) {
            if (!(*_loaded)[firstRow + i]) {
                _run[i] = Cell{morph::table::Unloaded{}};
            }
        }
        _out->cells(firstRow, _run);
    }

private:
    morph::table::ColumnSink* _out;
    std::vector<bool> const* _loaded;
    std::vector<Cell> _run;
};

class UnloadedSnapshot final : public morph::table::RowSnapshot {
public:
    UnloadedSnapshot(std::shared_ptr<morph::table::RowSnapshot const> inner,
                     std::shared_ptr<std::vector<bool> const> slow, std::shared_ptr<std::vector<bool> const> loaded)
        : _inner{std::move(inner)}, _slow{std::move(slow)}, _loaded{std::move(loaded)} {}

    [[nodiscard]] std::size_t rowCount() const override { return _inner->rowCount(); }
    [[nodiscard]] morph::table::RowId rowId(std::size_t row) const override { return _inner->rowId(row); }
    [[nodiscard]] Cell cell(std::size_t row, std::size_t column) const override {
        if ((*_slow)[column] && !(*_loaded)[row]) {
            return Cell{morph::table::Unloaded{}};
        }
        return _inner->cell(row, column);
    }
    void readColumn(std::size_t column, morph::table::ColumnSink& sink, std::size_t first,
                    std::size_t last) const override {
        if (!(*_slow)[column]) {
            _inner->readColumn(column, sink, first, last);
            return;
        }
        MaskingSink masking{sink, *_loaded};
        _inner->readColumn(column, masking, first, last);
    }

private:
    std::shared_ptr<morph::table::RowSnapshot const> _inner;
    std::shared_ptr<std::vector<bool> const> _slow;
    std::shared_ptr<std::vector<bool> const> _loaded;
};

}  // namespace

UnloadedSource::UnloadedSource(std::shared_ptr<morph::table::DataSource> source,
                               std::vector<std::string> const& columns, std::string level, double loadedFraction,
                               std::uint64_t seed)
    : _source{std::move(source)},
      _columns{_source->columns().begin(), _source->columns().end()},
      _loadedFraction{loadedFraction},
      _seed{seed} {
    if (level.empty()) {
        throw std::invalid_argument{"unloaded source: the base level cannot be unloaded"};
    }
    std::vector<bool> slow(_columns.size(), false);
    for (auto const& id : columns) {
        auto const at = std::ranges::find(_columns, id, &ColumnInfo::id);
        if (at == _columns.end()) {
            throw std::invalid_argument{std::format("unloaded source: no column {}", id)};
        }
        at->level = level;
        slow[static_cast<std::size_t>(at - _columns.begin())] = true;
    }
    _slow = std::make_shared<std::vector<bool> const>(std::move(slow));
    auto inner = _source->snapshot();
    std::vector<bool> loaded(inner->rowCount());
    for (std::size_t row = 0; row < loaded.size(); ++row) {
        loaded[row] = drawnLoaded(_seed, row, _loadedFraction);
    }
    _loaded = std::make_shared<std::vector<bool> const>(std::move(loaded));
    publish(std::move(inner));
    _source->subscribe(*this);
}

UnloadedSource::~UnloadedSource() { _source->unsubscribe(*this); }

std::span<ColumnInfo const> UnloadedSource::columns() const { return _columns; }

std::shared_ptr<morph::table::RowSnapshot const> UnloadedSource::snapshot() const { return _snapshot; }

void UnloadedSource::subscribe(morph::table::ChangeListener& listener) { _listeners.add(listener); }

void UnloadedSource::unsubscribe(morph::table::ChangeListener& listener) { _listeners.remove(listener); }

void UnloadedSource::load(std::span<std::size_t const> rows) {
    auto loaded = *_loaded;
    for (auto const row : rows) {
        if (row >= loaded.size()) {
            throw std::invalid_argument{std::format("unloaded source: no row {}", row)};
        }
        loaded[row] = true;
    }
    _loaded = std::make_shared<std::vector<bool> const>(std::move(loaded));
    publish(_source->snapshot());
    _listeners.notify(
        morph::table::RowChange{.kind = morph::table::ChangeKind::Updated, .rows = {rows.begin(), rows.end()}});
}

std::vector<std::size_t> UnloadedSource::unloadedRows() const {
    std::vector<std::size_t> out;
    for (std::size_t row = 0; row < _loaded->size(); ++row) {
        if (!(*_loaded)[row]) {
            out.push_back(row);
        }
    }
    return out;
}

void UnloadedSource::rowsChanged(morph::table::RowChange const& change) {
    auto inner = _source->snapshot();
    if (inner->rowCount() != _loaded->size()) {
        auto loaded = *_loaded;
        auto const kept = loaded.size();
        loaded.resize(inner->rowCount());
        for (std::size_t row = kept; row < loaded.size(); ++row) {
            loaded[row] = drawnLoaded(_seed, row, _loadedFraction);
        }
        _loaded = std::make_shared<std::vector<bool> const>(std::move(loaded));
    }
    publish(std::move(inner));
    _listeners.notify(change);
}

void UnloadedSource::publish(std::shared_ptr<morph::table::RowSnapshot const> inner) {
    _snapshot = std::make_shared<UnloadedSnapshot const>(std::move(inner), _slow, _loaded);
}

std::shared_ptr<UnloadedSource> withUnloaded(std::shared_ptr<morph::table::DataSource> source,
                                             std::vector<std::string> columns, std::string level,
                                             double loadedFraction, std::uint64_t seed) {
    return std::make_shared<UnloadedSource>(std::move(source), columns, std::move(level), loadedFraction, seed);
}

std::size_t processBytes() noexcept {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX counters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                             sizeof(counters)) != 0) {
        return counters.PrivateUsage;
    }
    return 0;
#elif defined(__linux__)
    try {
        std::ifstream statm{"/proc/self/statm"};
        std::size_t size = 0;
        std::size_t resident = 0;
        statm >> size >> resident;
        return resident * static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
    } catch (...) {
        return 0;
    }
#else
    return 0;
#endif
}

namespace {

// The model's process-wide state: the options, and each table's source once a call has built it.
struct WideModelState {
    std::mutex mutex;
    WideTableOptions wide{.rows = 100'000, .columns = 300, .seed = 1, .loadDelay = {}};
    WideTableOptions small{.rows = 1'000, .columns = 30, .seed = 1, .loadDelay = {}};
    std::shared_ptr<WideTableSource> wideSource;
    std::shared_ptr<WideTableSource> smallSource;
};

WideModelState& wideModelState() {
    static WideModelState state;
    return state;
}

// The source of one table, built under the state's mutex by the first call that needs it.
std::shared_ptr<WideTableSource> sourceOf(bool wide) {
    auto& state = wideModelState();
    std::scoped_lock const lock{state.mutex};
    auto& source = wide ? state.wideSource : state.smallSource;
    if (source == nullptr) {
        source = std::make_shared<WideTableSource>(wide ? state.wide : state.small);
    }
    return source;
}

// Sets both tables' seed and returns the sources built so far, building neither.
std::pair<std::shared_ptr<WideTableSource>, std::shared_ptr<WideTableSource>> builtSources(std::uint64_t seed) {
    auto& state = wideModelState();
    std::scoped_lock const lock{state.mutex};
    state.wide.seed = seed;
    state.small.seed = seed;
    return {state.wideSource, state.smallSource};
}

morph::table::TableDescription const& describedWide() {
    static auto const description = morph::table::describe(wideDescription("wide", 300));
    return description;
}

morph::table::TableDescription const& describedSmall() {
    static auto const description = morph::table::describe(wideDescription("small", 30));
    return description;
}

// `base` without `dropped`, from its columns and from its groups; a group left with fewer than two columns goes.
morph::table::TableDescription without(morph::table::TableDescription base, std::vector<std::string> const& dropped) {
    auto const isDropped = [&](std::string const& id) { return std::ranges::find(dropped, id) != dropped.end(); };
    std::erase_if(base.columns, [&](morph::table::TableColumn const& column) { return isDropped(column.info.id); });
    for (auto& group : base.groups) {
        std::erase_if(group.columns, isDropped);
    }
    std::erase_if(base.groups, [](morph::table::ColumnGroupInfo const& group) { return group.columns.size() < 2; });
    for (auto& column : base.columns) {
        if (!column.group.empty() &&
            std::ranges::find(base.groups, column.group, &morph::table::ColumnGroupInfo::id) == base.groups.end()) {
            column.group.clear();
        }
    }
    base.levels = levelsOf(base);
    base.id = "scoped";
    base.label = "scoped";
    return morph::table::describe(std::move(base));
}

std::vector<std::string> numbered(char prefix, int count) {
    std::vector<std::string> out;
    for (int i = 0; i < count; ++i) {
        out.push_back(std::format("{}{:03}", prefix, i));
    }
    return out;
}

morph::table::TableDescription const& scopedDescription(std::string_view principal) {
    static auto const extended = without(describedSmall(), {});
    static auto const common = without(describedSmall(), numbered('d', 6));
    static auto const restricted = [] {
        auto dropped = numbered('d', 6);
        auto texts = numbered('t', 7);
        dropped.insert(dropped.end(), texts.begin(), texts.end());
        return without(describedSmall(), dropped);
    }();
    if (principal == "extended") {
        return extended;
    }
    return principal == "restricted" ? restricted : common;
}

morph::table::ServedTable served(morph::table::TableDescription const& description,
                                 std::shared_ptr<WideTableSource> source) {
    morph::table::ServedTable out;
    out.description = description;
    out.source = std::move(source);
    return out;
}

}  // namespace

void WideTableModel::configure(WideTableOptions wide, WideTableOptions small) {
    auto& state = wideModelState();
    std::scoped_lock const lock{state.mutex};
    state.wide = wide;
    state.small = small;
    state.wideSource.reset();
    state.smallSource.reset();
}

morph::table::ServedTable WideTableModel::wide(morph::table::TableContext const& context) {
    static_cast<void>(context);
    return served(describedWide(), sourceOf(true));
}

morph::table::ServedTable WideTableModel::small(morph::table::TableContext const& context) {
    static_cast<void>(context);
    return served(describedSmall(), sourceOf(false));
}

morph::table::ServedTable WideTableModel::scoped(morph::table::TableContext const& context) {
    auto const& description = scopedDescription(context.session.principal);
    auto out = served(description, sourceOf(false));
    out.mayRead = [&description](std::string_view column) {
        return std::ranges::any_of(description.columns, [&](morph::table::TableColumn const& entry) {
            return entry.info.id == column;
        });
    };
    return out;
}

EditWideRowResult WideTableModel::execute(EditWideRow const& action) {
    EditWideRowResult result;
    morph::table::RowId const key{action.row + 1};
    auto const wideSource = sourceOf(true);
    wideSource->setCell(action.row, action.column, Cell{action.value});
    result.wide.rows.push_back(morph::table::ChangedRow{.key = key});
    auto const smallSource = sourceOf(false);
    auto const& columns = smallSource->columns();
    if (action.row < smallSource->options().rows &&
        std::ranges::find(columns, action.column, &ColumnInfo::id) != columns.end()) {
        smallSource->setCell(action.row, action.column, Cell{action.value});
        result.small.rows.push_back(morph::table::ChangedRow{.key = key});
    }
    return result;
}

bool WideTableModel::execute(ReplaceWideRows const& action) {
    // A source not built yet is built from the new seed.
    auto const [wideSource, smallSource] = builtSources(action.seed);
    if (wideSource != nullptr) {
        wideSource->regenerate(action.seed);
    }
    if (smallSource != nullptr) {
        smallSource->regenerate(action.seed);
    }
    return true;
}

std::uint64_t digestForTesting(WideTableSource const& source) {
    constexpr std::uint64_t kPrime = 1099511628211ULL;
    std::uint64_t hash = 14695981039346656037ULL;
    auto const mix = [&](std::string_view bytes) {
        for (char const byte : bytes) {
            hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(byte));
            hash *= kPrime;
        }
    };
    auto const snapshot = source.snapshot();
    auto const columns = source.columns().size();
    for (std::size_t row = 0; row < snapshot->rowCount(); ++row) {
        for (std::size_t column = 0; column < columns; ++column) {
            mix(morph::table::displayText(snapshot->cell(row, column)));
            mix("\x1f");
        }
    }
    return hash;
}

}  // namespace bench

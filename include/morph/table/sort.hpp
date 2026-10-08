// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file table/sort.hpp
/// @brief Typed sort keys and the chunked, stoppable merge sort over them.
///
/// Each column a sort or filter uses gets a `KeyColumn`: one typed array of
/// keys plus a `CellState` per cell, built once from a snapshot. Comparing two
/// rows is then a direct compare of two keys, with no parsing and no
/// allocation. Exact kinds stay exact: integers, dates and booleans compare as
/// `int64`, decimals and quantities as `Rational` (128-bit cross products);
/// only a `Number` column holds a double.
///
/// Ordering rules, normative for every client and server (spec 7 §5):
/// - an empty or invalid cell sorts after every valid one, in either direction;
/// - each key of a chain has its own direction;
/// - ties break by source row, so the sort is stable.
///
/// See `docs/spec/table/engine.md`.

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <compare>
#include <core/async/StopToken.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <morph/table/data_source.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace morph::table {

/// @brief A sort key's direction.
enum class SortDirection : std::uint8_t {
    Ascending,   ///< Smallest first.
    Descending,  ///< Largest first. Invalid cells still come last.
};

/// @brief One key of a sort chain.
struct SortKey {
    /// @brief The column's id.
    std::string column;
    /// @brief The key's direction.
    SortDirection dir = SortDirection::Ascending;

    /// @brief Member-wise equality.
    /// @param other The key to compare with.
    /// @return `true` when column and direction match.
    [[nodiscard]] bool operator==(SortKey const& other) const = default;
};

/// @brief A multi-key sort, most significant key first. Empty means source order.
using SortChain = std::vector<SortKey>;

/// @brief Whether a cell has a key.
enum class CellState : std::uint8_t {
    Valid,    ///< The cell has a key.
    Empty,    ///< The cell is empty; it sorts with invalid cells.
    Invalid,  ///< The cell could not be read for its column's kind.
};

/// @brief A deadline for one step of work; `kNoDeadline` runs to completion.
using Deadline = std::chrono::steady_clock::time_point;

/// @brief The deadline that never passes.
inline constexpr Deadline kNoDeadline = Deadline::max();

namespace detail {

// How many elements of work pass between two looks at the clock and the stop
// token: often enough to keep an owner step near its budget, rarely enough
// that the clock read is noise.
inline constexpr std::size_t kCheckEvery = 2048;

[[nodiscard]] inline bool shouldYield(Deadline deadline, ::core::async::StopToken const& stop) {
    return stop.stop_requested() || (deadline != kNoDeadline && std::chrono::steady_clock::now() >= deadline);
}

[[nodiscard]] inline std::optional<std::int64_t> parseInteger(std::string_view text) {
    std::int64_t value = 0;
    auto const* const end = text.data() + text.size();
    auto const [ptr, ec] = std::from_chars(text.data(), end, value);
    if (ec != std::errc{} || ptr != end || text.empty()) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] inline std::optional<double> parseReal(std::string_view text) {
    double value = 0;
    auto const* const end = text.data() + text.size();
    auto const [ptr, ec] = std::from_chars(text.data(), end, value);
    if (ec != std::errc{} || ptr != end || text.empty() || std::isnan(value)) {
        return std::nullopt;
    }
    return value;
}

}  // namespace detail

/// @brief One column's keys, built once and shared, immutable, between jobs.
///
/// Exactly one of the typed arrays is filled, chosen by the column's kind:
/// `ints` for `Integer`, `Key`, `Date`, `DateTime` and `Bool`; `exact` for
/// `Decimal` and `Quantity`; `reals` for `Number`; `text`, `folded` and `raw`
/// for `Text`; `cells` for a `Custom` column with a known comparator, and
/// `text` for one without (ordered by display text).
struct KeyColumn {
    /// @brief The column's kind.
    ColumnKind kind = ColumnKind::Text;
    /// @brief The state of each row's cell.
    std::vector<CellState> states;
    /// @brief Integer keys.
    std::vector<std::int64_t> ints;
    /// @brief Exact keys.
    std::vector<math::Rational> exact;
    /// @brief Floating-point keys.
    std::vector<double> reals;
    /// @brief Collation keys.
    std::vector<std::string> text;
    /// @brief Primary-strength folds, which text filters match.
    std::vector<std::string> folded;
    /// @brief The text as the source gave it, for case-sensitive filters.
    std::vector<std::string> raw;
    /// @brief Cells of a `Custom` column with a comparator.
    std::vector<Cell> cells;
    /// @brief The comparator of a `Custom` column, or empty.
    CellComparator comparator;

    /// @brief Whether this column orders `Custom` cells through a comparator.
    /// @return `true` when `comparator` is set.
    [[nodiscard]] bool usesComparator() const noexcept { return static_cast<bool>(comparator); }

    /// @brief An empty key column for @p info, sized for @p rows rows.
    /// @param info     The column.
    /// @param services Where a `Custom` column's comparator is looked up.
    /// @param rows     Row count.
    /// @return The column, every cell `Empty` until built.
    [[nodiscard]] static KeyColumn forColumn(ColumnInfo const& info, Services const& services, std::size_t rows) {
        KeyColumn out;
        out.kind = info.kind;
        out.states.assign(rows, CellState::Empty);
        switch (info.kind) {
            case ColumnKind::Integer:
            case ColumnKind::Key:
            case ColumnKind::Date:
            case ColumnKind::DateTime:
            case ColumnKind::Bool:
                out.ints.assign(rows, 0);
                break;
            case ColumnKind::Decimal:
            case ColumnKind::Quantity:
                out.exact.assign(rows, math::Rational{});
                break;
            case ColumnKind::Number:
                out.reals.assign(rows, 0.0);
                break;
            case ColumnKind::Text:
                out.text.resize(rows);
                out.folded.resize(rows);
                out.raw.resize(rows);
                break;
            case ColumnKind::Custom:
                if (auto const found = services.comparators.find(info.comparator);
                    found != services.comparators.end()) {
                    out.comparator = found->second;
                    out.cells.resize(rows);
                } else {
                    out.text.resize(rows);
                }
                break;
            default:
                break;
        }
        return out;
    }

    /// @brief Rebuilds the key of one row from @p cell.
    /// @param row      Row index.
    /// @param cell     The row's cell.
    /// @param info     The column.
    /// @param services Collator and date parser.
    void set(std::size_t row, Cell const& cell, ColumnInfo const& info, Services const& services) {
        states[row] = read(row, cell, info, services);
    }

private:
    [[nodiscard]] CellState read(std::size_t row, Cell const& cell, ColumnInfo const& info, Services const& services) {
        if (std::holds_alternative<std::monostate>(cell)) {
            return CellState::Empty;
        }
        auto const* const textCell = std::get_if<std::string>(&cell);
        if (textCell != nullptr && textCell->empty()) {
            return CellState::Empty;
        }
        switch (kind) {
            case ColumnKind::Integer:
            case ColumnKind::Key:
                return readInteger(row, cell);
            case ColumnKind::Decimal:
            case ColumnKind::Quantity:
                return readExact(row, cell);
            case ColumnKind::Number:
                return readReal(row, cell);
            case ColumnKind::Text: {
                auto const shown = textCell != nullptr ? *textCell : displayText(cell);
                auto const& collator = *services.collator;
                text[row] = collator.sortKey(shown);
                folded[row] = collator.fold(shown);
                raw[row] = shown;
                return CellState::Valid;
            }
            case ColumnKind::Date:
            case ColumnKind::DateTime:
                return readDate(row, cell, info, services);
            case ColumnKind::Bool:
                return readBool(row, cell);
            case ColumnKind::Custom:
                if (usesComparator()) {
                    cells[row] = cell;
                } else {
                    text[row] = services.collator->sortKey(displayText(cell));
                }
                return CellState::Valid;
            default:
                break;
        }
        return CellState::Invalid;
    }

    [[nodiscard]] CellState readInteger(std::size_t row, Cell const& cell) {
        if (auto const* value = std::get_if<std::int64_t>(&cell)) {
            ints[row] = *value;
            return CellState::Valid;
        }
        if (auto const* value = std::get_if<math::Rational>(&cell); value != nullptr && value->isInteger()) {
            ints[row] = value->numerator;
            return CellState::Valid;
        }
        if (auto const* value = std::get_if<std::string>(&cell)) {
            if (auto const parsed = detail::parseInteger(*value)) {
                ints[row] = *parsed;
                return CellState::Valid;
            }
        }
        return CellState::Invalid;
    }

    [[nodiscard]] CellState readExact(std::size_t row, Cell const& cell) {
        if (auto const* value = std::get_if<math::Rational>(&cell)) {
            exact[row] = *value;
            return CellState::Valid;
        }
        if (auto const* value = std::get_if<QuantityCell>(&cell)) {
            // Checked: a product that does not fit int64 is an invalid cell, not
            // a saturated value that would sort as the largest quantity.
            auto const canonical = math::checkedMul(value->amount, value->toCanonical);
            if (!canonical) {
                return CellState::Invalid;
            }
            exact[row] = *canonical;
            return CellState::Valid;
        }
        if (auto const* value = std::get_if<std::int64_t>(&cell)) {
            exact[row] = math::Rational{*value, math::DecimalPlaces{0}};
            return CellState::Valid;
        }
        if (auto const* value = std::get_if<std::string>(&cell)) {
            if (auto const parsed = parseDecimal(*value)) {
                exact[row] = *parsed;
                return CellState::Valid;
            }
        }
        return CellState::Invalid;
    }

    [[nodiscard]] CellState readReal(std::size_t row, Cell const& cell) {
        std::optional<double> value;
        if (auto const* real = std::get_if<double>(&cell)) {
            value = *real;
        } else if (auto const* integer = std::get_if<std::int64_t>(&cell)) {
            value = static_cast<double>(*integer);
        } else if (auto const* exactValue = std::get_if<math::Rational>(&cell)) {
            value = exactValue->toDouble(math::kMaxDecimalPlaces + 1);
        } else if (auto const* textValue = std::get_if<std::string>(&cell)) {
            value = detail::parseReal(*textValue);
        }
        if (!value || std::isnan(*value)) {
            return CellState::Invalid;
        }
        reals[row] = *value;
        return CellState::Valid;
    }

    [[nodiscard]] CellState readDate(std::size_t row, Cell const& cell, ColumnInfo const& info,
                                     Services const& services) {
        if (auto const* value = std::get_if<std::int64_t>(&cell)) {
            ints[row] = *value;
            return CellState::Valid;
        }
        if (auto const* value = std::get_if<std::string>(&cell)) {
            auto const& parser = *services.dates;
            auto const parsed = kind == ColumnKind::Date ? parser.parseDate(*value, info.format)
                                                         : parser.parseDateTime(*value, info.format);
            if (parsed) {
                ints[row] = *parsed;
                return CellState::Valid;
            }
        }
        return CellState::Invalid;
    }

    [[nodiscard]] CellState readBool(std::size_t row, Cell const& cell) {
        if (auto const* value = std::get_if<bool>(&cell)) {
            ints[row] = *value ? 1 : 0;
            return CellState::Valid;
        }
        if (auto const* value = std::get_if<std::int64_t>(&cell); value != nullptr && (*value == 0 || *value == 1)) {
            ints[row] = *value;
            return CellState::Valid;
        }
        if (auto const* value = std::get_if<std::string>(&cell)) {
            if (*value == "true" || *value == "false") {
                ints[row] = *value == "true" ? 1 : 0;
                return CellState::Valid;
            }
        }
        return CellState::Invalid;
    }
};

/// @brief Builds the keys of rows `[first, last)` of one column into @p out.
/// @param snapshot The rows.
/// @param column   Column index in the snapshot.
/// @param info     The column.
/// @param services Collator and date parser; a null member means its default.
/// @param first    First row.
/// @param last     One past the last row.
/// @param out      A column from `KeyColumn::forColumn` sized for the snapshot.
inline void buildKeys(RowSnapshot const& snapshot, std::size_t column, ColumnInfo const& info,
                      Services const& services, std::size_t first, std::size_t last, KeyColumn& out) {
    struct Sink final : ColumnSink {
        Sink(KeyColumn& keys, ColumnInfo const& column, Services const& with)
            : out{&keys}, info{&column}, services{&with} {}
        void cells(std::size_t firstRow, std::span<Cell const> run) override {
            for (std::size_t i = 0; i < run.size(); ++i) {
                out->set(firstRow + i, run[i], *info, *services);
            }
        }
        KeyColumn* out;
        ColumnInfo const* info;
        Services const* services;
    };
    if (services.collator == nullptr || services.dates == nullptr) {
        auto const filled = services.forTask();
        Sink sink{out, info, filled};
        snapshot.readColumn(column, sink, first, last);
        return;
    }
    Sink sink{out, info, services};
    snapshot.readColumn(column, sink, first, last);
}

/// @brief Compares two rows' keys in one column, valid before invalid in
///        either direction.
/// @param keys The column's keys.
/// @param a    One row.
/// @param b    Another row.
/// @param dir  The key's direction; it does not move invalid cells.
/// @return Negative, zero or positive as row @p a orders before, with, or after @p b.
[[nodiscard]] inline int compareKeys(KeyColumn const& keys, std::uint32_t a, std::uint32_t b, SortDirection dir) {
    bool const validA = keys.states[a] == CellState::Valid;
    bool const validB = keys.states[b] == CellState::Valid;
    if (validA != validB) {
        return validA ? -1 : 1;
    }
    if (!validA) {
        return 0;
    }
    auto const sign = [](auto ordering) -> int {
        if (ordering < 0) {
            return -1;
        }
        return ordering > 0 ? 1 : 0;
    };
    int result = 0;
    switch (keys.kind) {
        case ColumnKind::Integer:
        case ColumnKind::Key:
        case ColumnKind::Date:
        case ColumnKind::DateTime:
        case ColumnKind::Bool:
            result = sign(keys.ints[a] <=> keys.ints[b]);
            break;
        case ColumnKind::Decimal:
        case ColumnKind::Quantity:
            result = sign(keys.exact[a] <=> keys.exact[b]);
            break;
        case ColumnKind::Number:
            result = sign(std::strong_order(keys.reals[a], keys.reals[b]));
            break;
        case ColumnKind::Text:
            result = sign(keys.text[a].compare(keys.text[b]) <=> 0);
            break;
        case ColumnKind::Custom:
            result = keys.usesComparator() ? sign(keys.comparator(keys.cells[a], keys.cells[b]))
                                           : sign(keys.text[a].compare(keys.text[b]) <=> 0);
            break;
        default:
            break;
    }
    return dir == SortDirection::Descending ? -result : result;
}

/// @brief A sort chain resolved to key columns: the comparator of a sort.
///
/// Holds pointers to key columns it does not own; whoever builds it keeps the
/// columns alive for as long as it is used.
class RowOrder {
public:
    /// @brief One resolved key.
    struct Key {
        /// @brief The column's keys.
        KeyColumn const* keys = nullptr;
        /// @brief The key's direction.
        SortDirection dir = SortDirection::Ascending;
    };

    /// @brief Source order.
    RowOrder() = default;

    /// @brief An order over resolved keys, most significant first.
    /// @param keys The keys.
    explicit RowOrder(std::vector<Key> keys) : _keys{std::move(keys)} {}

    /// @brief Whether row @p a orders before row @p b. Ties break by source
    ///        row, so this is a strict total order and every sort is stable.
    /// @param a One row.
    /// @param b Another row.
    /// @return `true` when @p a comes first.
    [[nodiscard]] bool operator()(std::uint32_t a, std::uint32_t b) const {
        for (auto const& key : _keys) {
            if (int const result = compareKeys(*key.keys, a, b, key.dir); result != 0) {
                return result < 0;
            }
        }
        return a < b;
    }

    /// @brief Whether this is source order.
    /// @return `true` when no key is set.
    [[nodiscard]] bool empty() const noexcept { return _keys.empty(); }

private:
    std::vector<Key> _keys;
};

/// @brief A bottom-up merge sort over row indices that can stop and resume.
///
/// Runs of `kRun` rows are sorted first, then merged pass by pass. `run`
/// returns at a deadline or a stop request and continues from there on the
/// next call, which is how the engine sorts 100,000 rows on an owner thread
/// in frame-sized steps, or abandons a superseded sort on a worker.
class ChunkedMergeSort {
public:
    /// @brief Rows per initial run.
    static constexpr std::size_t kRun = 512;

    /// @brief Prepares to sort @p rows by @p order.
    /// @param rows  Row indices to sort.
    /// @param order The comparator; its key columns must outlive the sort.
    ChunkedMergeSort(std::vector<std::uint32_t> rows, RowOrder order)
        : _rows{std::move(rows)}, _order{std::move(order)} {
        _buffer.resize(_rows.size());
    }

    /// @brief Sorts until done, the deadline passes, or a stop is requested.
    /// @param deadline When to yield.
    /// @param stop     A stop request ends the step early.
    /// @return `true` when the rows are sorted.
    bool run(Deadline deadline, ::core::async::StopToken const& stop) {
        auto const count = _rows.size();
        std::size_t work = 0;
        while (_runStart < count) {
            auto const end = std::min(_runStart + kRun, count);
            std::sort(_rows.begin() + static_cast<std::ptrdiff_t>(_runStart),
                      _rows.begin() + static_cast<std::ptrdiff_t>(end), _order);
            work += end - _runStart;
            _runStart = end;
            if (work >= detail::kCheckEvery && detail::shouldYield(deadline, stop)) {
                return false;
            }
        }
        while (_width < count) {
            while (_mergeAt < count) {
                if (!_merging) {
                    _left = _mergeAt;
                    _leftEnd = std::min(_mergeAt + _width, count);
                    _right = _leftEnd;
                    _rightEnd = std::min(_mergeAt + (2 * _width), count);
                    _out = _mergeAt;
                    _merging = true;
                }
                while (_left < _leftEnd && _right < _rightEnd) {
                    _buffer[_out++] = _order(_rows[_right], _rows[_left]) ? _rows[_right++] : _rows[_left++];
                    if (++work % detail::kCheckEvery == 0 && detail::shouldYield(deadline, stop)) {
                        return false;
                    }
                }
                while (_left < _leftEnd) {
                    _buffer[_out++] = _rows[_left++];
                }
                while (_right < _rightEnd) {
                    _buffer[_out++] = _rows[_right++];
                }
                _merging = false;
                _mergeAt = _rightEnd;
            }
            _rows.swap(_buffer);
            _width *= 2;
            _mergeAt = 0;
            if (detail::shouldYield(deadline, stop) && _width < count) {
                return false;
            }
        }
        return true;
    }

    /// @brief Whether the sort has finished.
    /// @return `true` once `run` returned `true`.
    [[nodiscard]] bool done() const noexcept { return _runStart >= _rows.size() && _width >= _rows.size(); }

    /// @brief The sorted rows. Valid once `done()`.
    /// @return The rows, moved out.
    [[nodiscard]] std::vector<std::uint32_t> take() && { return std::move(_rows); }

private:
    std::vector<std::uint32_t> _rows;
    std::vector<std::uint32_t> _buffer;
    RowOrder _order;
    std::size_t _runStart = 0;
    std::size_t _width = kRun;
    std::size_t _mergeAt = 0;
    bool _merging = false;
    std::size_t _left = 0;
    std::size_t _leftEnd = 0;
    std::size_t _right = 0;
    std::size_t _rightEnd = 0;
    std::size_t _out = 0;
};

/// @brief Sorts @p rows by @p order in one call.
/// @param rows  Row indices.
/// @param order The comparator.
/// @return The sorted rows.
[[nodiscard]] inline std::vector<std::uint32_t> sortRows(std::vector<std::uint32_t> rows, RowOrder order) {
    ChunkedMergeSort sort{std::move(rows), std::move(order)};
    static_cast<void>(sort.run(kNoDeadline, ::core::async::StopToken{}));
    return std::move(sort).take();
}

}  // namespace morph::table

/// @brief JSON names of `morph::table::SortDirection`: `asc` and `desc`.
template <>
struct glz::meta<morph::table::SortDirection> {
    using enum morph::table::SortDirection;
    /// @brief Each enumerator after its JSON name.
    static constexpr auto value = glz::enumerate("asc", Ascending, "desc", Descending);
};

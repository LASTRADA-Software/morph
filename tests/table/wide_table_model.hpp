// SPDX-License-Identifier: Apache-2.0
//
// The wide-table fixture every table measurement runs against (docs/spec/table/table.md, "Performance"): a
// column store generated from a seed, read through a `DataSource`. A cell depends only on the seed, its column's
// id and its row, so the same seed gives the same table on every machine, and the 300- and 30-column layouts agree
// on the rows and columns they share.

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <morph/table/data_source.hpp>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bench {

// What a column's cells are made of; it decides the generator and the storage.
enum class WideRole : std::uint8_t { Key, Integer, Decimal, Quantity, Measure, Text, LongText, Date, DateTime, Bool };

struct WideColumn {
    morph::table::ColumnInfo info;  // id and kind; Measure-role columns are ColumnKind::Decimal
    WideRole role = WideRole::Integer;
    bool sparse = false;  // 10% of its cells empty
    std::string level;    // "recipe" or empty
    std::string group;    // "g3", "g16" or empty
};

struct WideTableOptions {
    std::int64_t rows = 100'000;
    int columns = 300;  // 300 or 30; anything else throws std::invalid_argument
    std::uint64_t seed = 1;
    std::chrono::microseconds loadDelay{0};  // the slow level's wait per key
};

// The bits a cell is generated from: splitmix64(splitmix64(seed ^ fnv1a64(column)) + row).
[[nodiscard]] std::uint64_t cellBits(std::uint64_t seed, std::string_view column, std::int64_t row) noexcept;

// The 300- or 30-column layout, in declared order; throws std::invalid_argument for any other count.
[[nodiscard]] std::vector<WideColumn> wideColumns(int columns);

// The bytes a cell costs held as a `Cell`: the variant, plus a string's heap buffer when it has one.
[[nodiscard]] std::size_t approximateCellBytes(morph::table::Cell const& cell) noexcept;

namespace detail {
struct ColumnData;
}

// The fixture's rows as a `DataSource`. The row key of row r is r + 1, which is also the `id` column's value.
//
// snapshot(), setCell() and regenerate() hold one mutex around reading or swapping the snapshot pointer, and notify
// listeners after releasing it, so many clients' strands may share one source. subscribe() and unsubscribe() are
// owner-affine, as for every source.
class WideTableSource final : public morph::table::DataSource {
public:
    explicit WideTableSource(WideTableOptions options);

    [[nodiscard]] std::span<morph::table::ColumnInfo const> columns() const override;
    [[nodiscard]] std::shared_ptr<morph::table::RowSnapshot const> snapshot() const override;
    void subscribe(morph::table::ChangeListener& listener) override;
    void unsubscribe(morph::table::ChangeListener& listener) override;

    [[nodiscard]] std::vector<WideColumn> const& layout() const noexcept;
    [[nodiscard]] WideTableOptions const& options() const noexcept;
    // Bytes of the column arrays, capacity included.
    [[nodiscard]] std::size_t residentBytes() const noexcept;

    // Replaces one cell: copies that one column, so a snapshot taken before keeps the old value; notifies Updated.
    // The value is the column's own alternative, or empty; a decimal keeps the column's places. Anything else throws
    // std::invalid_argument.
    void setCell(std::int64_t row, std::string_view column, morph::table::Cell value);

    // Generates every column again from `seed`; notifies Reset.
    void regenerate(std::uint64_t seed);

private:
    WideTableOptions _options;
    std::vector<WideColumn> _layout;
    std::vector<morph::table::ColumnInfo> _columns;
    mutable std::mutex _mutex;
    std::shared_ptr<morph::table::RowSnapshot const> _snapshot;
    std::vector<std::shared_ptr<detail::ColumnData const>> _data;
    morph::table::detail::Listeners _listeners;
};

// The process's private bytes (Windows) or resident bytes (Linux); 0 where neither is available. What the memory
// baselines difference before and after the thing they measure.
[[nodiscard]] std::size_t processBytes() noexcept;

// FNV-1a 64 over displayText(cell) of every row and column of the source, row by row, each followed by '\x1f'.
[[nodiscard]] std::uint64_t digestForTesting(WideTableSource const& source);

}  // namespace bench

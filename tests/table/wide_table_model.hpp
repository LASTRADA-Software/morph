// SPDX-License-Identifier: Apache-2.0
//
// The wide-table fixture every table measurement runs against (docs/spec/table/table.md, "Performance"): a
// column store generated from a seed, read through a `DataSource`. A cell depends only on the seed, its column's
// id and its row, so the same seed gives the same table on every machine, and the 300- and 30-column layouts agree
// on the rows and columns they share.
//
// `WideTableModel` serves it as a registered model with three tables, so the table actions are measured and tested
// over the bridge. Its registrations are in this header: a translation unit that names one of its tables'
// `TableOf` or `ChangedRows` needs the table's registration in sight.

#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <morph/core/bridge.hpp>
#include <morph/core/effects.hpp>
#include <morph/core/registry.hpp>
#include <morph/table/data_source.hpp>
#include <morph/table/description.hpp>
#include <morph/table/level_cache.hpp>
#include <morph/table/row_selection.hpp>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bench {

// What a column's cells are made of; it decides the generator and the storage.
enum class WideRole : std::uint8_t { Key, Integer, Decimal, Quantity, Measure, Text, LongText, Date, DateTime, Bool };

struct WideColumn {
    morph::table::ColumnInfo info;  // id, kind and level; Measure-role columns are ColumnKind::Measure
    WideRole role = WideRole::Integer;
    bool sparse = false;  // 10% of its cells empty
    std::string level;    // "recipe" or empty
    std::string group;    // "g3", "g16" or empty
};

struct WideTableOptions {
    std::int64_t rows = 100'000;
    int columns = 300;  // 300 or 30; anything else throws std::invalid_argument
    std::uint64_t seed = 1;
    std::chrono::microseconds loadDelay{0};  // the recipe loader sleeps this long per key
    // The model's `recipe` level cache: the fraction of rows stored when a table's source is built (rows chosen by
    // (seed, row)), the keys one loader call takes, and whether the loader returns no value for any key. The cache
    // is shared by `wide` and `small` and takes the smaller fraction and batch of the two, and fails if either
    // says so.
    double recipePreloaded = 1.0;
    std::size_t recipeBatch = 64;
    bool recipeFails = false;
};

// The fixture's unit registry, one for the process: mass kg 1, g 1/1000, mg 1/1,000,000, t 1000; volume l 1,
// ml 1/1000, m3 1000; ratio "" 1, percent 1/100, permille 1/1000. A Measure-role cell's unit is drawn from its bits.
[[nodiscard]] std::shared_ptr<morph::table::UnitRegistry const> const& wideUnits();

// The bits a cell is generated from: splitmix64(splitmix64(seed ^ fnv1a64(column)) + row).
[[nodiscard]] std::uint64_t cellBits(std::uint64_t seed, std::string_view column, std::int64_t row) noexcept;

// The 300- or 30-column layout, in declared order; throws std::invalid_argument for any other count.
[[nodiscard]] std::vector<WideColumn> wideColumns(int columns);

// The declared groups of a layout, in the order it first names them: `g3` ("Group of 3": t004 to t006) and, in the
// 300-column layout, `g16` ("Group of 16": t011 to t026). What WideTableSource::groups() returns.
[[nodiscard]] std::vector<morph::table::ColumnGroupInfo> wideGroups(std::vector<WideColumn> const& layout);

// The description of wideColumns(columns): `id` as given, `label` = `id`, each column's label its id, `group` from
// the layout and `groups` the source's (wideGroups), key "id",
// `levels` the levels its columns name (`recipe`), `units` those of wideUnits() its measures allow (all ten),
// `revision` empty.
[[nodiscard]] morph::table::TableDescription wideDescription(std::string id, int columns);

// The bytes a cell costs held as a `Cell`: the variant, plus a string's heap buffer when it has one.
[[nodiscard]] std::size_t approximateCellBytes(morph::table::Cell const& cell) noexcept;

namespace detail {
struct ColumnData;
}

// The fixture's rows as a `DataSource`. The row key of row r is r + 1, which is also the `id` column's value.
//
// snapshot() and options() hold one mutex only around reading the snapshot pointer or the options, and setCell() and
// regenerate() only around swapping them: a writer builds its new columns before it takes that mutex, under a second
// one that orders writers, so a reader never waits for a column to be copied or generated. Listeners are notified
// after both are released, so many clients' strands may share one source. subscribe() and unsubscribe() are
// owner-affine, as for every source.
class WideTableSource final : public morph::table::DataSource {
public:
    explicit WideTableSource(WideTableOptions options);

    [[nodiscard]] std::span<morph::table::ColumnInfo const> columns() const override;
    [[nodiscard]] std::shared_ptr<morph::table::RowSnapshot const> snapshot() const override;
    void subscribe(morph::table::ChangeListener& listener) override;
    void unsubscribe(morph::table::ChangeListener& listener) override;
    // wideGroups(layout()).
    [[nodiscard]] std::span<morph::table::ColumnGroupInfo const> groups() const override;

    [[nodiscard]] std::vector<WideColumn> const& layout() const noexcept;
    // The registry its measures are numbered in (wideUnits()): what every sort, filter and page over it is given.
    [[nodiscard]] std::shared_ptr<morph::table::UnitRegistry const> units() const;
    // A copy, since regenerate() changes the seed.
    [[nodiscard]] WideTableOptions options() const;
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
    std::vector<morph::table::ColumnGroupInfo> _groups;
    std::mutex _writer;
    mutable std::mutex _mutex;
    std::shared_ptr<morph::table::RowSnapshot const> _snapshot;
    std::vector<std::shared_ptr<detail::ColumnData const>> _data;
    morph::table::detail::Listeners _listeners;
};

// A source over another whose named columns are at one level, and read `Unloaded` in the rows not loaded yet: the
// slow level of a benchmark or a test, without a level cache. Whether a row starts loaded depends on (seed, row)
// alone, so a seed gives the same rows on every machine.
//
// load() marks rows loaded, publishes one new snapshot and notifies one `Updated` for all of them: an arrival. A
// change of the wrapped source is forwarded after the new snapshot is published, a row beyond the old count starting
// as the draw says. A slow column's runs are handed through readColumn as the wrapped source hands them, its
// unloaded rows replaced. Owner-affine, as every source.
class UnloadedSource final : public morph::table::DataSource, private morph::table::ChangeListener {
public:
    // Throws std::invalid_argument when `columns` names a column `source` does not have, or `level` is empty.
    UnloadedSource(std::shared_ptr<morph::table::DataSource> source, std::vector<std::string> const& columns,
                   std::string const& level, double loadedFraction, std::uint64_t seed);
    ~UnloadedSource() override;
    UnloadedSource(UnloadedSource const&) = delete;
    UnloadedSource& operator=(UnloadedSource const&) = delete;
    UnloadedSource(UnloadedSource&&) = delete;
    UnloadedSource& operator=(UnloadedSource&&) = delete;

    [[nodiscard]] std::span<morph::table::ColumnInfo const> columns() const override;
    [[nodiscard]] std::shared_ptr<morph::table::RowSnapshot const> snapshot() const override;
    void subscribe(morph::table::ChangeListener& listener) override;
    void unsubscribe(morph::table::ChangeListener& listener) override;
    // The wrapped source's.
    [[nodiscard]] std::span<morph::table::ColumnGroupInfo const> groups() const override;

    // Marks `rows` loaded; a row out of range throws std::invalid_argument before anything changes.
    void load(std::span<std::size_t const> rows);
    // The rows still unloaded, ascending.
    [[nodiscard]] std::vector<std::size_t> unloadedRows() const;

private:
    void rowsChanged(morph::table::RowChange const& change) override;
    void publish(std::shared_ptr<morph::table::RowSnapshot const> inner);

    std::shared_ptr<morph::table::DataSource> _source;
    std::vector<morph::table::ColumnInfo> _columns;
    std::shared_ptr<std::vector<bool> const> _slow;  // by column index
    double _loadedFraction;
    std::uint64_t _seed;
    std::shared_ptr<std::vector<bool> const> _loaded;  // by row
    std::shared_ptr<morph::table::RowSnapshot const> _snapshot;
    morph::table::detail::Listeners _listeners;
};

// `source` with `columns` at `level`, `loadedFraction` of its rows (by the draw of (seed, row)) loaded.
[[nodiscard]] std::shared_ptr<UnloadedSource> withUnloaded(std::shared_ptr<morph::table::DataSource> source,
                                                           std::vector<std::string> const& columns,
                                                           std::string const& level, double loadedFraction,
                                                           std::uint64_t seed);

// The process's private bytes (Windows) or resident bytes (Linux); 0 where neither is available. What the memory
// baselines difference before and after the thing they measure.
[[nodiscard]] std::size_t processBytes() noexcept;

// FNV-1a 64 over displayText(cell) of every row and column of the source, row by row, each followed by '\x1f'.
[[nodiscard]] std::uint64_t digestForTesting(WideTableSource const& source);

inline constexpr std::string_view kWideModelType = "WideTableModel";

// Holds the recipe loader: a held gate makes each loader call wait until it is released.
class LoadGate {
public:
    // Every loader call from now on waits.
    void hold();
    // Every call passes, waiting ones included.
    void release();
    // `calls` loader calls pass, waiting ones first; then the gate holds again.
    void release(std::size_t calls);
    // Waits until the gate lets this call pass.
    void pass();

private:
    std::mutex _mutex;
    std::condition_variable _changed;
    bool _held = false;
    std::size_t _passes = 0;
};

// The `recipe` level's columns: d000..d009 and t001..t010 of the wide layout, in that order.
[[nodiscard]] std::vector<morph::table::ColumnInfo> recipeColumns();

// Sets one integer cell: `row` is the row index (its key is row + 1), `column` an integer column of the wide layout.
struct EditWideRow {
    std::int64_t row = 0;
    std::string column;
    std::int64_t value = 0;
};

// Generates every row of both tables again from `seed`.
struct ReplaceWideRows {
    std::uint64_t seed = 1;
};

struct EditWideRowResult;

// The fixture as a model: tables `wide` (the 300-column layout), `small` (the 30-column one) and `scoped` (small's
// rows, its columns by principal). Its state is process-wide, so every instance serves the same rows.
class WideTableModel {
public:
    // Replaces both tables' options and drops both sources; each is built again, lazily, by the first table call
    // that needs it. A request already holding a source keeps it. Callable at any time, from any test case.
    static void configure(WideTableOptions wide, WideTableOptions small);

    // wideDescription("wide", 300) over one shared WideTableSource.
    [[nodiscard]] morph::table::ServedTable wide(morph::table::TableContext const& context);
    // wideDescription("small", 30) over one shared WideTableSource.
    [[nodiscard]] morph::table::ServedTable small(morph::table::TableContext const& context);
    // small's rows; its description is small's without d000..d005 for every principal but "extended" (all 30),
    // and for "restricted" also without t000..t006, which leaves it no level. A column outside the caller's
    // description is not readable. Its scope is the caller's principal.
    [[nodiscard]] morph::table::ServedTable scoped(morph::table::TableContext const& context);

    // Sets the cell in `wide`, and in `small` when the row and the column are there; names the row in each.
    [[nodiscard]] EditWideRowResult execute(EditWideRow const& action);
    // Regenerates both tables from the seed; the recipe cache is emptied and preloaded again at the new seed.
    bool execute(ReplaceWideRows const& action);

    // The recipe loader's gate; open unless a test holds it.
    [[nodiscard]] static LoadGate& recipeGate();
    // The keys of every recipe loader call since the cache was built, one entry per call.
    [[nodiscard]] static std::vector<std::vector<morph::table::RowId>> loaderCalls();
    // The recipe level's cache, built with the first source (and again by `configure`).
    [[nodiscard]] static std::shared_ptr<morph::table::CellLevelCache> recipeCache();
};

struct EditWideRowResult {
    morph::table::ChangedRows<&WideTableModel::wide> wide;
    morph::table::ChangedRows<&WideTableModel::small> small;
};

}  // namespace bench

BRIDGE_REGISTER_MODEL(bench::WideTableModel, "WideTableModel")
BRIDGE_REGISTER_TABLE(bench::WideTableModel, "wide", &bench::WideTableModel::wide)
BRIDGE_REGISTER_TABLE(bench::WideTableModel, "small", &bench::WideTableModel::small)
BRIDGE_REGISTER_TABLE(bench::WideTableModel, "scoped", &bench::WideTableModel::scoped)
BRIDGE_REGISTER_ACTION(bench::WideTableModel, bench::EditWideRow, "EditWideRow")
BRIDGE_REGISTER_ACTION(bench::WideTableModel, bench::ReplaceWideRows, "ReplaceWideRows")
BRIDGE_REGISTER_EFFECTS(bench::EditWideRow, morph::effects::updatesTable<&bench::WideTableModel::wide>,
                        morph::effects::updatesTable<&bench::WideTableModel::small>)
BRIDGE_REGISTER_EFFECTS(bench::ReplaceWideRows, morph::effects::changesTable<&bench::WideTableModel::wide>,
                        morph::effects::changesTable<&bench::WideTableModel::small>)

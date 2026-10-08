// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file table/engine.hpp
/// @brief The table engine: sort, filter and view mapping, on and off the owner.
///
/// An `Engine` owns a table's view: the sort chain, the filter, and the
/// mapping between source rows and view rows. It is owner-affine — every
/// member is called on the owner thread — and toolkit-free: a renderer applies
/// the `ViewChange` it emits as list-model operations.
///
/// **Execution.** Every recomputation is a job over an immutable snapshot of
/// the rows, the key columns it may reuse, and the compiled sort and filter.
/// A job never touches the engine. A small table (below
/// `EngineOptions::smallTable` rows), or an engine with no owner executor,
/// computes in one step inside the call. Otherwise the job runs on the worker
/// executor, or, with no worker, in frame-budgeted steps posted to the owner;
/// `pending()` is true until its result is applied on the owner, and the
/// previous view stays visible meanwhile. One job runs at a time: a request
/// while one runs stops it and queues; the stopped job's result is dropped.
///
/// **Changes.** Updated rows re-key only themselves and are merged back into
/// the order, once per owner turn however many updates arrived. Inserted,
/// removed and reset rows recompute the view. With `ReorderPolicy::Deferred`
/// an updated row keeps its place until the view has been quiet for the
/// settle interval, or until its edit ends.
///
/// See `docs/spec/table/engine.md`.

#include <algorithm>
#include <chrono>
#include <core/async/StopToken.hpp>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <limits>
#include <memory>
#include <morph/core/callback_scope.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/profiler.hpp>
#include <morph/table/data_source.hpp>
#include <morph/table/filter.hpp>
#include <morph/table/sort.hpp>
#include <morph/table/view_change.hpp>
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace morph::table {

// NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access) -- row and column indices are bounded by the snapshot's own counts, and the sort and filter loops index once per comparison, where at() would check every access

/// @brief What an updated row that would move does.
enum class ReorderPolicy : std::uint8_t {
    Immediate,  ///< It moves at once.
    Deferred,   ///< It keeps its place until the view is quiet for `settle`, or its edit ends.
};

/// @brief How an engine runs and what it is given.
struct EngineOptions {
    /// @brief Where results are applied. Null: every computation runs inside the
    ///        call that asks for it (what `table::apply` uses on a server).
    exec::IExecutor* owner = nullptr;
    /// @brief Where jobs run off the owner. Null: jobs run in frame-budgeted
    ///        steps posted to `owner`. Must outlive every job the engine starts.
    exec::IExecutor* worker = nullptr;
    /// @brief Tables with fewer rows compute on the owner in one step.
    std::size_t smallTable = 2000;
    /// @brief A `ViewChange` with more operations than this is one `Reset`.
    std::size_t resetThreshold = 5000;
    /// @brief The longest an owner step runs when there is no worker.
    std::chrono::microseconds frameBudget{8000};
    /// @brief Collator, date parser, comparators and progress sink.
    Services services;
    /// @brief What an updated row that would move does.
    ReorderPolicy reorder = ReorderPolicy::Immediate;
    /// @brief The quiet interval after which deferred rows move.
    std::chrono::milliseconds settle{500};
    /// @brief Runs a callback on the owner after a delay; the engine uses it for
    ///        the settle interval. Empty: deferred rows move only on
    ///        `settleNow()` or `endEdit()`.
    std::function<void(std::chrono::milliseconds, std::function<void()>)> scheduleSettle;
};

/// @brief Counters for tests, benchmarks and profiler plots.
struct EngineStats {
    /// @brief Jobs started (one-step runs included).
    std::size_t jobs = 0;
    /// @brief Sorts of every source row.
    std::size_t fullSorts = 0;
    /// @brief Sorts of a filtered subset.
    std::size_t subsetSorts = 0;
    /// @brief Key columns built from scratch.
    std::size_t keyBuilds = 0;
    /// @brief Repairs of updated rows.
    std::size_t repairs = 0;
    /// @brief Results dropped because a newer request superseded them.
    std::size_t dropped = 0;
    /// @brief Steps run on the owner thread (one-step runs included).
    std::size_t ownerSteps = 0;
    /// @brief The longest single step on the owner thread, in microseconds.
    std::size_t maxOwnerStepMicros = 0;
};

/// @brief Column order and visibility, kept apart from the engine's view.
///
/// At least one column stays visible.
class ColumnLayout {
public:
    /// @brief A layout of @p ids, all visible, in that order.
    /// @param ids Column ids, the document's default order.
    explicit ColumnLayout(std::vector<std::string> ids) : _defaults{std::move(ids)} { reset(); }

    /// @brief The columns in display order.
    /// @return Column ids.
    [[nodiscard]] std::span<std::string const> order() const noexcept { return _order; }

    /// @brief The visible columns in display order.
    /// @return Column ids.
    [[nodiscard]] std::vector<std::string> visible() const {
        std::vector<std::string> out;
        for (auto const& columnId : _order) {
            if (!_hidden.contains(columnId)) {
                out.push_back(columnId);
            }
        }
        return out;
    }

    /// @brief Shows or hides a column.
    /// @param columnId   Column columnId.
    /// @param show `true` to show.
    /// @return `false` when the column is unknown, or hiding it would leave none visible.
    bool setVisible(std::string const& columnId, bool show) {
        if (std::ranges::find(_order, columnId) == _order.end()) {
            return false;
        }
        if (show) {
            _hidden.erase(columnId);
            return true;
        }
        if (!_hidden.contains(columnId) && visible().size() == 1) {
            return false;
        }
        _hidden.insert(columnId);
        return true;
    }

    /// @brief Moves a column to a display position.
    /// @param columnId Column columnId.
    /// @param position Position in `order()`, clamped to the end.
    /// @return `false` when the column is unknown.
    bool move(std::string const& columnId, std::size_t position) {
        auto const found = std::ranges::find(_order, columnId);
        if (found == _order.end()) {
            return false;
        }
        auto moved = std::move(*found);
        _order.erase(found);
        _order.insert(_order.begin() + static_cast<std::ptrdiff_t>(std::min(position, _order.size())),
                      std::move(moved));
        return true;
    }

    /// @brief Restores the default order with every column visible.
    void reset() {
        _order = _defaults;
        _hidden.clear();
    }

    /// @brief Adopts a new column list: kept columns keep their place and
    ///        visibility, new ones are appended visible, vanished ones dropped.
    /// @param ids The new columns, as the new default order.
    void rebuild(std::vector<std::string> ids) {
        std::vector<std::string> order;
        for (auto const& columnId : _order) {
            if (std::ranges::find(ids, columnId) != ids.end()) {
                order.push_back(columnId);
            }
        }
        for (auto const& columnId : ids) {
            if (std::ranges::find(order, columnId) == order.end()) {
                order.push_back(columnId);
            }
        }
        std::erase_if(_hidden,
                      [&](std::string const& hiddenId) { return std::ranges::find(ids, hiddenId) == ids.end(); });
        _defaults = std::move(ids);
        _order = std::move(order);
        if (!_order.empty() && visible().empty()) {
            _hidden.erase(_order.front());
        }
    }

private:
    std::vector<std::string> _defaults;
    std::vector<std::string> _order;
    std::unordered_set<std::string> _hidden;
};

namespace detail {

/// @brief Everything one view job reads. Immutable once the job starts.
struct JobInput {
    /// @brief The rows.
    std::shared_ptr<RowSnapshot const> snapshot;
    /// @brief The columns.
    std::vector<ColumnInfo> columns;
    /// @brief Services for this job alone (`Services::forTask`).
    Services services;
    /// @brief Key columns by column index; null when not built.
    std::vector<std::shared_ptr<KeyColumn const>> keys;
    /// @brief Columns whose keys the job needs.
    std::vector<std::size_t> needed;
    /// @brief The sort chain.
    SortChain sort;
    /// @brief The compiled filter, or null.
    std::shared_ptr<CompiledFilter const> filter;
    /// @brief The full source's order for `sort` on `snapshot`, or null.
    std::shared_ptr<std::vector<std::uint32_t> const> cachedSorted;
    /// @brief The displayed view the change is computed against.
    std::vector<std::uint32_t> previousView;
    /// @brief The snapshot `previousView`'s rows index.
    std::shared_ptr<RowSnapshot const> previousSnapshot;
    /// @brief `previousView`'s rows index `snapshot` directly.
    bool indicesStable = true;
    /// @brief Repair: the rows whose cells changed.
    std::vector<std::uint32_t> dirty;
    /// @brief Repair: the sorted, filtered order, without holds, that the dirty rows merge into.
    std::vector<std::uint32_t> previousTrueView;
    /// @brief The job repairs updated rows rather than recomputing.
    bool repair = false;
    /// @brief Every surviving row may have new content (a reset).
    bool allChanged = false;
    /// @brief More operations than this make the change a reset.
    std::size_t resetThreshold = 5000;
};

/// @brief What a finished job hands back to the owner.
struct JobResult {
    /// @brief Key columns by column index, for `JobInput::snapshot`.
    std::vector<std::shared_ptr<KeyColumn const>> keys;
    /// @brief A full order for the chain, to cache, or null.
    std::shared_ptr<std::vector<std::uint32_t> const> sorted;
    /// @brief The sorted, filtered view.
    std::vector<std::uint32_t> view;
    /// @brief From `JobInput::previousView` to `view`.
    ViewChange change;
    /// @brief Every source row was sorted.
    bool fullSort = false;
    /// @brief Only the filter's survivors were sorted.
    bool subsetSort = false;
    /// @brief Updated rows were merged into the previous order.
    bool repaired = false;
    /// @brief Key columns built from scratch.
    std::size_t keyBuilds = 0;
};

/// @brief One recomputation of a view, runnable in steps.
///
/// Phases: build missing keys (or patch the keys of dirty rows), then either
/// repair the previous order, or filter and sort, then diff against the
/// previous view. `step` returns at a deadline or a stop request and resumes
/// where it stopped.
class ViewJob {
public:
    /// @brief Prepares a job.
    /// @param input Everything it reads.
    explicit ViewJob(JobInput input) : _in{std::move(input)} {
        _result.keys = _in.keys;
        _result.keys.resize(_in.columns.size());
    }

    /// @brief Runs until done, the deadline, or a stop request.
    /// @param deadline When to yield.
    /// @param stop     A stop request ends the step.
    /// @return `true` when the job is done.
    bool step(Deadline deadline, ::core::async::StopToken const& stop) {
        while (_phase != Phase::Done) {
            if (stop.stop_requested()) {
                return false;
            }
            bool finished = false;
            switch (_phase) {
                case Phase::Keys:
                    finished = buildKeys(deadline, stop);
                    break;
                case Phase::Order:
                    finished = order(deadline, stop);
                    break;
                case Phase::Diff:
                    diff();
                    finished = true;
                    break;
                case Phase::Done:
                default:
                    finished = true;
                    break;
            }
            if (!finished) {
                return false;
            }
            advance();
            if (_phase != Phase::Done && detail::shouldYield(deadline, stop)) {
                return false;
            }
        }
        return true;
    }

    /// @brief Phases done and total, for a progress sink.
    /// @return (done, total).
    [[nodiscard]] std::pair<std::size_t, std::size_t> progress() const noexcept {
        return {static_cast<std::size_t>(_phase), static_cast<std::size_t>(Phase::Done)};
    }

    /// @brief Whether every phase has run.
    /// @return `true` when done.
    [[nodiscard]] bool done() const noexcept { return _phase == Phase::Done; }

    /// @brief What the job reads.
    /// @return The input.
    [[nodiscard]] JobInput const& input() const noexcept { return _in; }

    /// @brief Moves the result out. Valid once `done()`; the input stays readable.
    /// @return The result.
    [[nodiscard]] JobResult takeResult() { return std::move(_result); }

private:
    enum class Phase : std::uint8_t { Keys, Order, Diff, Done };

    void advance() {
        switch (_phase) {
            case Phase::Keys:
                _phase = Phase::Order;
                break;
            case Phase::Order:
                _phase = Phase::Diff;
                break;
            case Phase::Diff:
            case Phase::Done:
            default:
                _phase = Phase::Done;
                break;
        }
    }

    // Builds every needed column that has no keys, and in a repair patches the
    // dirty rows of every column that has them (on a copy: the old keys may
    // still be read by the owner or another job).
    bool buildKeys(Deadline deadline, ::core::async::StopToken const& stop) {
        MORPH_ZONE("table.keyBuild");
        auto const rows = _in.snapshot->rowCount();
        if (!_patched && _in.repair) {
            for (std::size_t column = 0; column < _result.keys.size(); ++column) {
                if (_result.keys[column] == nullptr) {
                    continue;
                }
                auto copy = std::make_shared<KeyColumn>(*_result.keys[column]);
                for (auto const row : _in.dirty) {
                    copy->set(row, _in.snapshot->cell(row, column), _in.columns[column], _in.services);
                }
                _result.keys[column] = std::move(copy);
            }
            _patched = true;
        }
        while (_neededAt < _in.needed.size()) {
            auto const column = _in.needed[_neededAt];
            if (_result.keys[column] != nullptr && _building == nullptr) {
                ++_neededAt;
                continue;
            }
            if (_building == nullptr) {
                _building = std::make_shared<KeyColumn>(KeyColumn::forColumn(_in.columns[column], _in.services, rows));
                _rowAt = 0;
            }
            while (_rowAt < rows) {
                auto const end = std::min(_rowAt + detail::kCheckEvery, rows);
                morph::table::buildKeys(*_in.snapshot, column, _in.columns[column], _in.services, _rowAt, end,
                                        *_building);
                _rowAt = end;
                if (_rowAt < rows && detail::shouldYield(deadline, stop)) {
                    return false;
                }
            }
            _result.keys[column] = std::move(_building);
            _building = nullptr;
            ++_result.keyBuilds;
            ++_neededAt;
        }
        return true;
    }

    // Zone text for a profiler capture: the sort chain, and the filter's size.
    [[nodiscard]] std::string describeSort() const {
        std::string out;
        for (auto const& key : _in.sort) {
            out += key.column;
            out += key.dir == SortDirection::Descending ? " desc " : " asc ";
        }
        return out;
    }
    [[nodiscard]] std::string describeFilter() const {
        return _in.filter == nullptr ? std::string{}
                                     : std::to_string(_in.filter->columns().size()) + " columns, " +
                                           std::to_string(_in.filter->groups().size()) + " groups";
    }

    [[nodiscard]] RowOrder rowOrder() const {
        std::vector<RowOrder::Key> keys;
        for (auto const& key : _in.sort) {
            if (auto const column = findColumn(_in.columns, key.column)) {
                keys.push_back({.keys = _result.keys[*column].get(), .dir = key.dir});
            }
        }
        return RowOrder{std::move(keys)};
    }

    [[nodiscard]] std::vector<KeyColumn const*> keyPointers() const {
        std::vector<KeyColumn const*> out;
        out.reserve(_result.keys.size());
        for (auto const& keys : _result.keys) {
            out.push_back(keys.get());
        }
        return out;
    }

    [[nodiscard]] bool passesFilter(std::uint32_t row) const {
        if (_in.filter == nullptr) {
            return true;
        }
        for (auto const& column : _in.filter->columns()) {
            if (!CompiledFilter::passes(column, *_result.keys[column.column], row)) {
                return false;
            }
        }
        for (auto const& group : _in.filter->groups()) {
            bool const any = std::ranges::any_of(group.columns, [&](CompiledFilter::Column const& column) {
                return CompiledFilter::passes(column, *_result.keys[column.column], row);
            });
            if (!any) {
                return false;
            }
        }
        return true;
    }

    // Merges the dirty rows back into an order the clean rows still satisfy.
    [[nodiscard]] std::vector<std::uint32_t> repairOrder(std::span<std::uint32_t const> previous,
                                                         std::vector<std::uint32_t> dirtyRows,
                                                         RowOrder const& orderBy) const {
        std::vector<bool> isDirty(_in.snapshot->rowCount(), false);
        for (auto const row : _in.dirty) {
            isDirty[row] = true;
        }
        std::vector<std::uint32_t> clean;
        clean.reserve(previous.size());
        for (auto const row : previous) {
            if (!isDirty[row]) {
                clean.push_back(row);
            }
        }
        auto const sortedDirty = sortRows(std::move(dirtyRows), orderBy);
        std::vector<std::uint32_t> out(clean.size() + sortedDirty.size());
        std::ranges::merge(clean, sortedDirty, out.begin(), orderBy);
        return out;
    }

    // Merges the re-keyed dirty rows into the previous order (and the cached
    // full order), without sorting the rest.
    void repair() {
        MORPH_ZONE("table.change");
        auto const orderBy = rowOrder();
        std::vector<std::uint32_t> passing;
        for (auto const row : _in.dirty) {
            if (passesFilter(row)) {
                passing.push_back(row);
            }
        }
        _result.view = repairOrder(_in.previousTrueView, std::move(passing), orderBy);
        if (_in.cachedSorted != nullptr) {
            _result.sorted =
                std::make_shared<std::vector<std::uint32_t> const>(repairOrder(*_in.cachedSorted, _in.dirty, orderBy));
        }
        _result.repaired = true;
    }

    // Chooses the work: filter the cached sorted order; or filter, then sort
    // the survivors; or sort every row and cache the order.
    void plan() {
        auto const rows = _in.snapshot->rowCount();
        auto const filtering = _in.filter != nullptr && !_in.filter->empty();
        _started = true;
        if (_in.cachedSorted != nullptr) {
            _base = *_in.cachedSorted;
            _sorted = true;
        } else {
            _base.resize(rows);
            std::ranges::iota(_base, 0U);
            _sorted = _in.sort.empty();
            if (!_sorted && !filtering) {
                _sort.emplace(std::move(_base), rowOrder());
                _result.fullSort = true;
            }
        }
        if (filtering && (_sorted || _sort == std::nullopt)) {
            _filterRun.emplace(*_in.filter, keyPointers(), std::move(_base));
        }
    }

    bool order(Deadline deadline, ::core::async::StopToken const& stop) {
        if (_in.repair) {
            repair();
            return true;
        }
        if (!_started) {
            plan();
        }
        if (_filterRun) {
            MORPH_ZONE("table.filter");
            MORPH_ZONE_TEXT(describeFilter());
            if (!_filterRun->run(deadline, stop)) {
                return false;
            }
            _base = std::move(*_filterRun).take();
            _filterRun.reset();
            if (!_sorted) {
                // Filter first, then sort only the survivors.
                _sort.emplace(std::move(_base), rowOrder());
                _result.subsetSort = true;
            }
        }
        if (_sort) {
            MORPH_ZONE("table.sort");
            MORPH_ZONE_TEXT(describeSort());
            if (!_sort->run(deadline, stop)) {
                return false;
            }
            _base = std::move(*_sort).take();
            _sort.reset();
            _sorted = true;
            if (_result.fullSort) {
                _result.sorted = std::make_shared<std::vector<std::uint32_t> const>(_base);
            }
        }
        if (_in.cachedSorted != nullptr && !_in.sort.empty()) {
            _result.sorted = _in.cachedSorted;
        }
        _result.view = std::move(_base);
        return true;
    }

    void diff() {
        MORPH_ZONE("table.apply");
        std::vector<std::uint32_t> changed;
        if (_in.repair) {
            changed = _in.dirty;
        }
        if (_in.indicesStable) {
            if (_in.allChanged) {
                changed = _result.view;
            }
            _result.change =
                diffViews(_in.previousView, _result.view, changed, _in.snapshot->rowCount(), _in.resetThreshold);
            return;
        }
        // Rows of the previous view map to new rows by key; vanished rows get
        // identities past the new snapshot's rows.
        auto const rows = _in.snapshot->rowCount();
        std::unordered_map<RowId, std::uint32_t> byKey;
        byKey.reserve(rows);
        for (std::size_t row = 0; row < rows; ++row) {
            byKey.emplace(_in.snapshot->rowId(row), static_cast<std::uint32_t>(row));
        }
        std::vector<std::uint32_t> previous;
        previous.reserve(_in.previousView.size());
        auto next = static_cast<std::uint32_t>(rows);
        for (auto const row : _in.previousView) {
            auto const found = byKey.find(_in.previousSnapshot->rowId(row));
            previous.push_back(found != byKey.end() ? found->second : next++);
        }
        if (_in.allChanged) {
            changed = _result.view;
        }
        _result.change = diffViews(previous, _result.view, changed, next, _in.resetThreshold);
    }

    JobInput _in;
    JobResult _result;
    Phase _phase = Phase::Keys;
    // key build
    bool _patched = false;
    std::size_t _neededAt = 0;
    std::size_t _rowAt = 0;
    std::shared_ptr<KeyColumn> _building;
    // order
    bool _started = false;
    bool _sorted = false;
    std::vector<std::uint32_t> _base;
    std::optional<FilterRun> _filterRun;
    std::optional<ChunkedMergeSort> _sort;
};

}  // namespace detail

/// @brief Sorts, filters and maps the view of one table.
///
/// Owner-affine; see the file comment for how it executes. Holds a shared
/// reference to its source and subscribes to its changes for its lifetime.
class Engine final : private ChangeListener {
public:
    /// @brief Builds the engine; the initial view is every row in source order.
    /// @param source  The rows. Must not be null.
    /// @param options How the engine runs.
    explicit Engine(std::shared_ptr<DataSource> source, EngineOptions options = {})
        : _source{std::move(source)},
          _options{std::move(options)},
          _columns(_source->columns().begin(), _source->columns().end()),
          _snapshot{_source->snapshot()},
          _viewSnapshot{_snapshot},
          _keys(_columns.size()),
          _view(identity(_snapshot->rowCount())),
          _trueView{_view} {
        _source->subscribe(*this);
    }

    Engine(Engine const&) = delete;
    Engine& operator=(Engine const&) = delete;
    Engine(Engine&&) = delete;
    Engine& operator=(Engine&&) = delete;

    /// @brief Stops a running job and unsubscribes from the source.
    ~Engine() override {
        _jobStop.request_stop();
        _source->unsubscribe(*this);
    }

    /// @brief Sets the sort chain; an empty chain is source order.
    /// @param chain The keys, most significant first.
    /// @return `UnknownColumn` when a key names no column; the sort is then unchanged.
    std::expected<void, TableError> setSort(SortChain chain) {
        for (auto const& key : chain) {
            if (!detail::findColumn(_columns, key.column)) {
                return std::unexpected(TableError{.code = TableErrorCode::UnknownColumn,
                                                  .column = key.column,
                                                  .message = "no column " + key.column});
            }
        }
        if (chain == _sort) {
            return {};
        }
        _sort = std::move(chain);
        _holds.clear();
        request(Request::Full);
        return {};
    }

    /// @brief Sets the filter.
    /// @param spec The filter; an empty spec shows every row.
    /// @return The compile error, naming its column; the filter is then unchanged.
    std::expected<void, TableError> setFilter(FilterSpec spec) {
        auto compiled = compileFilter(spec, _columns, _options.services);
        if (!compiled) {
            return std::unexpected(compiled.error());
        }
        _filterSpec = std::move(spec);
        _filter = std::make_shared<CompiledFilter const>(std::move(*compiled));
        _holds.clear();
        request(Request::Full);
        return {};
    }

    /// @brief The current sort chain.
    /// @return The chain.
    [[nodiscard]] SortChain const& sort() const noexcept { return _sort; }

    /// @brief The current filter.
    /// @return The filter spec.
    [[nodiscard]] FilterSpec const& filter() const noexcept { return _filterSpec; }

    /// @brief The table's columns.
    /// @return The columns.
    [[nodiscard]] std::span<ColumnInfo const> columns() const noexcept { return _columns; }

    /// @brief Rows in the view.
    /// @return View row count.
    [[nodiscard]] std::size_t viewRowCount() const noexcept { return _view.size(); }

    /// @brief Rows in the source the view was computed from.
    /// @return Source row count.
    [[nodiscard]] std::size_t sourceRowCount() const { return _viewSnapshot->rowCount(); }

    /// @brief The source row shown at a view row.
    /// @param viewRow View row index.
    /// @return The source row, or nothing past the end.
    [[nodiscard]] std::optional<std::size_t> sourceRowOf(std::size_t viewRow) const {
        if (viewRow >= _view.size()) {
            return std::nullopt;
        }
        return _view[viewRow];
    }

    /// @brief The view row showing a source row.
    /// @param sourceRow Source row index.
    /// @return The view row, or nothing when the row is filtered out or past the end.
    [[nodiscard]] std::optional<std::size_t> viewRowOf(std::size_t sourceRow) const {
        indexView();
        if (sourceRow >= _viewIndex.size() || _viewIndex[sourceRow] == kAbsent) {
            return std::nullopt;
        }
        return _viewIndex[sourceRow];
    }

    /// @brief The key of the row at a view row.
    /// @param viewRow View row index, below `viewRowCount()`.
    /// @return The row's key.
    [[nodiscard]] RowId rowIdAt(std::size_t viewRow) const { return _viewSnapshot->rowId(_view.at(viewRow)); }

    /// @brief The view row of the row keyed @p id.
    /// @param key A row key.
    /// @return The view row, or nothing when no row in the view has that key.
    [[nodiscard]] std::optional<std::size_t> viewRowOfKey(RowId const& key) const {
        indexKeys();
        auto const found = _keyIndex.find(key);
        return found == _keyIndex.end() ? std::nullopt : std::optional{found->second};
    }

    /// @brief One cell of the view.
    /// @param viewRow View row index, below `viewRowCount()`.
    /// @param column  Column index.
    /// @return The cell.
    [[nodiscard]] Cell cellAt(std::size_t viewRow, std::size_t column) const {
        return _viewSnapshot->cell(_view.at(viewRow), column);
    }

    /// @brief The view as source rows.
    /// @return Source row indices in view order.
    [[nodiscard]] std::span<std::uint32_t const> view() const noexcept { return _view; }

    /// @brief The snapshot the view's rows index.
    /// @return The snapshot.
    [[nodiscard]] std::shared_ptr<RowSnapshot const> const& viewSnapshot() const noexcept { return _viewSnapshot; }

    /// @brief Whether a computation is in flight.
    /// @return `true` from an asynchronous request until its result is applied.
    [[nodiscard]] bool pending() const noexcept { return _pending; }

    /// @brief Sets the receiver of view changes, called on the owner after the
    ///        view changed.
    /// @param handler The receiver; empty to stop receiving.
    void onViewChange(std::function<void(ViewChange const&)> handler) { _onViewChange = std::move(handler); }

    /// @brief Sets the receiver of `pending()` changes.
    /// @param handler The receiver; empty to stop receiving.
    void onPending(std::function<void(bool)> handler) { _onPending = std::move(handler); }

    /// @brief Counters.
    /// @return The engine's counters so far.
    [[nodiscard]] EngineStats const& stats() const noexcept { return _stats; }

    /// @brief Marks a row as being edited: with `Deferred` reorder it keeps its
    ///        place through updates until `endEdit`.
    /// @param key The row's key.
    void beginEdit(RowId const& key) { _editing.insert(key); }

    /// @brief Ends an edit; a held row moves to its sorted place now.
    /// @param key The row's key.
    void endEdit(RowId const& key) {
        _editing.erase(key);
        if (_holds.erase(key) > 0) {
            relayout();
        }
    }

    /// @brief Releases every deferred row not being edited, as the settle
    ///        interval does.
    void settleNow() {
        std::erase_if(_holds, [&](RowId const& key) { return !_editing.contains(key); });
        relayout();
    }

    /// @brief Rows currently held in place by `Deferred` reorder.
    /// @return Their keys.
    [[nodiscard]] std::vector<RowId> heldRows() const { return {_holds.begin(), _holds.end()}; }

private:
    static constexpr auto kAbsent = std::numeric_limits<std::uint32_t>::max();

    [[nodiscard]] static std::vector<std::uint32_t> identity(std::size_t rows) {
        std::vector<std::uint32_t> out(rows);
        std::ranges::iota(out, 0U);
        return out;
    }

    enum class Request : std::uint8_t { Full, Repair };

    void rowsChanged(RowChange const& change) override {
        switch (change.kind) {
            case ChangeKind::Updated:
                for (auto const row : change.rows) {
                    _dirty.push_back(static_cast<std::uint32_t>(row));
                }
                break;
            case ChangeKind::Reset:
                _columnsChanged = true;
                _structural = true;
                break;
            case ChangeKind::Inserted:
            case ChangeKind::Removed:
            default:
                _structural = true;
                break;
        }
        ++_settleSeq;
        if (_options.owner == nullptr) {
            flushChanges();
            return;
        }
        if (!_flushScheduled) {
            _flushScheduled = true;
            _options.owner->post(_alive.guard([this] { flushChanges(); }));
        }
    }

    // Applies every change notified since the last flush: one repair for any
    // number of updates, or a recompute after a structural change.
    void flushChanges() {
        MORPH_ZONE("table.snapshot");
        _flushScheduled = false;
        _snapshot = _source->snapshot();
        if (_columnsChanged) {
            adoptColumns();
        }
        if (_structural || _running || _needFull || _dirty.empty()) {
            _keys.assign(_columns.size(), nullptr);
            _sortCache = {};
            _dirty.clear();
            _holds.clear();
            bool const allChanged = _columnsChanged || _structural;
            _structural = false;
            _columnsChanged = false;
            _needFull = false;
            request(Request::Full, allChanged);
            return;
        }
        request(Request::Repair);
    }

    // Re-reads the columns after a reset, keeping the sort keys and filter
    // entries whose columns remain.
    void adoptColumns() {
        _columns.assign(_source->columns().begin(), _source->columns().end());
        std::erase_if(_sort, [&](SortKey const& key) { return !detail::findColumn(_columns, key.column); });
        std::erase_if(_filterSpec.columns,
                      [&](auto const& entry) { return !detail::findColumn(_columns, entry.first).has_value(); });
        for (auto& group : _filterSpec.groups) {
            std::erase_if(group.columns,
                          [&](std::string const& columnId) { return !detail::findColumn(_columns, columnId); });
        }
        std::erase_if(_filterSpec.groups, [](GroupFilter const& group) { return group.columns.empty(); });
        auto compiled = compileFilter(_filterSpec, _columns, _options.services);
        if (compiled) {
            _filter = std::make_shared<CompiledFilter const>(std::move(*compiled));
        } else {
            // A remaining entry no longer compiles for its column's new kind.
            _filterSpec = {};
            _filter = nullptr;
        }
    }

    [[nodiscard]] detail::JobInput makeInput(Request kind, bool allChanged) {
        detail::JobInput input;
        input.snapshot = _snapshot;
        input.columns = _columns;
        input.services = _options.services.forTask();
        input.keys = _keys;
        input.keys.resize(_columns.size());
        for (auto const& key : _sort) {
            if (auto const index = detail::findColumn(_columns, key.column)) {
                input.needed.push_back(*index);
            }
        }
        if (_filter != nullptr) {
            for (auto const column : _filter->columnsUsed()) {
                input.needed.push_back(column);
            }
        }
        std::ranges::sort(input.needed);
        auto const [first, last] = std::ranges::unique(input.needed);
        input.needed.erase(first, last);
        input.sort = _sort;
        input.filter = _filter;
        if (!_sort.empty() && _sortCache.order != nullptr && _sortCache.chain == _sort &&
            _sortCache.snapshot == _snapshot) {
            input.cachedSorted = _sortCache.order;
        }
        input.previousView = _view;
        input.previousSnapshot = _viewSnapshot;
        input.repair = kind == Request::Repair;
        input.indicesStable = input.repair || _viewSnapshot == _snapshot;
        if (input.repair) {
            input.dirty = std::move(_dirty);
            std::ranges::sort(input.dirty);
            auto const [dupFirst, dupLast] = std::ranges::unique(input.dirty);
            input.dirty.erase(dupFirst, dupLast);
            input.previousTrueView = _trueView;
            if (_sortCache.order != nullptr && _sortCache.chain == _sort) {
                input.cachedSorted = _sortCache.order;
            }
        }
        _dirty.clear();
        input.allChanged = allChanged;
        input.resetThreshold = _options.resetThreshold;
        return input;
    }

    void request(Request kind, bool allChanged = false) {
        if (_running) {
            // One job at a time: stop the running one; the next starts from the
            // latest state when it ends. Its result is dropped, so a queued
            // repair could not merge into it: recompute instead.
            _jobStop.request_stop();
            _queued = true;
            if (kind == Request::Repair) {
                _needFull = true;
            }
            _queuedAllChanged = _queuedAllChanged || allChanged;
            return;
        }
        auto const rows = _snapshot->rowCount();
        if (_options.owner == nullptr || rows < _options.smallTable) {
            runNow(kind, allChanged);
            return;
        }
        start(kind, allChanged);
    }

    void runNow(Request kind, bool allChanged) {
        auto const began = std::chrono::steady_clock::now();
        ++_stats.jobs;
        detail::ViewJob job{makeInput(kind, allChanged)};
        static_cast<void>(job.step(kNoDeadline, ::core::async::StopToken{}));
        recordOwnerStep(began);
        apply(job.input(), job.takeResult());
    }

    void start(Request kind, bool allChanged) {
        _running = true;
        _queued = false;
        _queuedAllChanged = false;
        ++_stats.jobs;
        ++_generation;
        _jobStop = ::core::async::StopSource{};
        setPending(true);
        auto const job = std::make_shared<detail::ViewJob>(makeInput(kind, allChanged));
        auto const generation = _generation;
        auto* const owner = _options.owner;
        if (_options.worker == nullptr) {
            owner->post(_alive.guard([this, job, generation] { ownerStep(generation, job); }));
            return;
        }
        // The worker task never names the engine: it holds the job and two
        // callbacks gated on the engine's lifetime, which run on the owner.
        auto finished = _alive.guard([this, job, generation] { finish(generation, job); });
        std::function<void(std::size_t, std::size_t)> report;
        if (auto const progress = _options.services.progress) {
            report =
                _alive.guard([progress](std::size_t done, std::size_t total) { progress->progress(done, total); });
        }
        _options.worker->post([job, stop = _jobStop.get_token(), owner, finished = std::move(finished),
                               report = std::move(report)] mutable {
            static_cast<void>(job->step(kNoDeadline, stop));
            if (report) {
                auto const [done, total] = job->progress();
                owner->post([report, done = done, total = total] { report(done, total); });
            }
            owner->post(std::move(finished));
        });
    }

    void ownerStep(std::uint64_t generation, std::shared_ptr<detail::ViewJob> const& job) {
        auto const began = std::chrono::steady_clock::now();
        auto const stop = _jobStop.get_token();
        bool const done = job->step(began + _options.frameBudget, stop);
        recordOwnerStep(began);
        if (_options.services.progress != nullptr) {
            auto const [doneUnits, total] = job->progress();
            _options.services.progress->progress(doneUnits, total);
        }
        if (!done && !stop.stop_requested() && generation == _generation) {
            _options.owner->post(_alive.guard([this, generation, job] { ownerStep(generation, job); }));
            return;
        }
        finish(generation, job);
    }

    void finish(std::uint64_t generation, std::shared_ptr<detail::ViewJob> const& job) {
        if (generation != _generation) {
            return;
        }
        _running = false;
        if (_queued || !job->done()) {
            ++_stats.dropped;
            if (job->input().repair) {
                _needFull = true;
            }
            bool const allChanged = _queuedAllChanged;
            if (_needFull) {
                _keys.assign(_columns.size(), nullptr);
                _sortCache = {};
                _needFull = false;
            }
            _queued = false;
            _queuedAllChanged = false;
            if (_snapshot->rowCount() < _options.smallTable) {
                setPending(false);
                runNow(Request::Full, allChanged);
                return;
            }
            start(Request::Full, allChanged);
            return;
        }
        auto const began = std::chrono::steady_clock::now();
        apply(job->input(), job->takeResult());
        recordOwnerStep(began);
        setPending(false);
    }

    void apply(detail::JobInput const& input, detail::JobResult result) {
        _stats.keyBuilds += result.keyBuilds;
        _stats.fullSorts += result.fullSort ? 1 : 0;
        _stats.subsetSorts += result.subsetSort ? 1 : 0;
        _stats.repairs += result.repaired ? 1 : 0;
        if (input.snapshot == _snapshot) {
            _keys = std::move(result.keys);
            if (result.sorted != nullptr) {
                _sortCache =
                    SortCache{.chain = input.sort, .snapshot = input.snapshot, .order = std::move(result.sorted)};
            }
        }
        MORPH_PLOT("table.sourceRows", input.snapshot->rowCount());
        MORPH_PLOT("table.viewRows", result.view.size());
        MORPH_PLOT("table.keyCacheColumns",
                   std::ranges::count_if(_keys, [](auto const& keys) { return keys != nullptr; }));
        _trueView = std::move(result.view);
        _viewSnapshot = input.snapshot;
        auto change = std::move(result.change);
        if (_options.reorder == ReorderPolicy::Deferred && input.repair) {
            for (auto const row : input.dirty) {
                if (std::ranges::find(input.previousView, row) != input.previousView.end()) {
                    _holds.insert(input.snapshot->rowId(row));
                }
            }
            for (auto const& editing : _editing) {
                _holds.insert(editing);
            }
            if (!_holds.empty()) {
                auto const displayed = holdLayout(input.previousView);
                change = diffViews(input.previousView, displayed, input.dirty, input.snapshot->rowCount(),
                                   _options.resetThreshold);
                _view = displayed;
                scheduleSettle();
                publish(change);
                return;
            }
        }
        _view = _trueView;
        publish(change);
    }

    // The true view with every held row put back at its displayed index.
    [[nodiscard]] std::vector<std::uint32_t> holdLayout(std::span<std::uint32_t const> displayed) const {
        std::vector<std::pair<std::size_t, std::uint32_t>> held;  // (displayed index, row)
        std::vector<bool> isHeld(_viewSnapshot->rowCount(), false);
        for (std::size_t i = 0; i < displayed.size(); ++i) {
            auto const row = displayed[i];
            if (row < isHeld.size() && _holds.contains(_viewSnapshot->rowId(row))) {
                held.emplace_back(i, row);
                isHeld[row] = true;
            }
        }
        std::vector<std::uint32_t> out;
        out.reserve(_trueView.size() + held.size());
        for (auto const row : _trueView) {
            if (!isHeld[row]) {
                out.push_back(row);
            }
        }
        for (auto const& [index, row] : held) {
            out.insert(out.begin() + static_cast<std::ptrdiff_t>(std::min(index, out.size())), row);
        }
        return out;
    }

    void relayout() {
        auto const displayed = _holds.empty() ? _trueView : holdLayout(_view);
        auto const change = diffViews(_view, displayed, {}, _viewSnapshot->rowCount(), _options.resetThreshold);
        _view = displayed;
        publish(change);
    }

    void scheduleSettle() {
        if (!_options.scheduleSettle) {
            return;
        }
        auto const seq = _settleSeq;
        _options.scheduleSettle(_options.settle, _alive.guard([this, seq] {
            if (seq == _settleSeq) {
                settleNow();
            }
        }));
    }

    void publish(ViewChange const& change) {
        _viewIndexValid = false;
        _keyIndexValid = false;
        if (!change.empty() && _onViewChange) {
            _onViewChange(change);
        }
    }

    void setPending(bool value) {
        if (_pending == value) {
            return;
        }
        _pending = value;
        if (_onPending) {
            _onPending(value);
        }
    }

    void recordOwnerStep(std::chrono::steady_clock::time_point began) {
        auto const micros = static_cast<std::size_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - began).count());
        _stats.maxOwnerStepMicros = std::max(_stats.maxOwnerStepMicros, micros);
        ++_stats.ownerSteps;
    }

    void indexView() const {
        if (_viewIndexValid) {
            return;
        }
        _viewIndex.assign(_viewSnapshot->rowCount(), kAbsent);
        for (std::size_t i = 0; i < _view.size(); ++i) {
            _viewIndex[_view[i]] = static_cast<std::uint32_t>(i);
        }
        _viewIndexValid = true;
    }

    void indexKeys() const {
        if (_keyIndexValid) {
            return;
        }
        _keyIndex.clear();
        _keyIndex.reserve(_view.size());
        for (std::size_t i = 0; i < _view.size(); ++i) {
            _keyIndex.emplace(_viewSnapshot->rowId(_view[i]), i);
        }
        _keyIndexValid = true;
    }

    struct SortCache {
        SortChain chain;
        std::shared_ptr<RowSnapshot const> snapshot;
        std::shared_ptr<std::vector<std::uint32_t> const> order;
    };

    std::shared_ptr<DataSource> _source;
    EngineOptions _options;
    std::vector<ColumnInfo> _columns;
    std::shared_ptr<RowSnapshot const> _snapshot;      // what the next job computes from
    std::shared_ptr<RowSnapshot const> _viewSnapshot;  // what `_view` indexes
    std::vector<std::shared_ptr<KeyColumn const>> _keys;
    SortCache _sortCache;
    SortChain _sort;
    FilterSpec _filterSpec;
    std::shared_ptr<CompiledFilter const> _filter;
    std::vector<std::uint32_t> _view;      // displayed
    std::vector<std::uint32_t> _trueView;  // sorted and filtered, without holds
    mutable std::vector<std::uint32_t> _viewIndex;
    mutable bool _viewIndexValid = false;
    mutable std::unordered_map<RowId, std::size_t> _keyIndex;
    mutable bool _keyIndexValid = false;
    // changes
    std::vector<std::uint32_t> _dirty;
    bool _structural = false;
    bool _columnsChanged = false;
    bool _flushScheduled = false;
    bool _needFull = false;
    // deferred reorder
    std::unordered_set<RowId> _holds;
    std::unordered_set<RowId> _editing;
    std::uint64_t _settleSeq = 0;
    // execution
    bool _pending = false;
    bool _running = false;
    bool _queued = false;
    bool _queuedAllChanged = false;
    std::uint64_t _generation = 0;
    ::core::async::StopSource _jobStop;
    std::function<void(ViewChange const&)> _onViewChange;
    std::function<void(bool)> _onPending;
    EngineStats _stats;
    async::CallbackScope _alive;
};

// NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)

}  // namespace morph::table

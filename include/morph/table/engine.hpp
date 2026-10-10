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
/// that makes the running job's result useless (a new sort, filter or
/// structural change) stops it and its result is dropped; updated rows wait
/// for it and are repaired after.
///
/// **Changes.** Updated rows re-key only themselves and are merged back into
/// the order, once per owner turn however many updates arrived. Inserted,
/// removed and reset rows recompute the view. Work a dropped job had taken
/// (updated rows, rows owed a `Changed`) is kept for the next one. With
/// `ReorderPolicy::Deferred` an updated row keeps its place until the view has
/// been quiet for the settle interval, or until its edit ends.
///
/// **Callbacks.** Every handler the engine calls (`onViewChange`, `onPending`,
/// `onError`, the progress sink, the settle scheduler) may call the engine
/// again or destroy it: the engine calls out last, or checks that it is still
/// alive before it goes on.
///
/// See `docs/spec/table/engine.md`.

#include <algorithm>
#include <chrono>
#include <core/async/StopToken.hpp>
#include <cstddef>
#include <cstdint>
#include <exception>
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
    /// @brief Rows whose cells changed since `keys` were built: their keys are
    ///        rebuilt on copies, a repair merges them back, and the change
    ///        reports those in both views as `Changed`.
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
    ///
    /// An exception from an injected service (a collator, date parser or
    /// comparator) ends the job: it is done, with `failure()` set.
    /// @param deadline When to yield.
    /// @param stop     A stop request ends the step.
    /// @return `true` when the job is done.
    bool step(Deadline deadline, ::core::async::StopToken const& stop) {
        try {
            return run(deadline, stop);
        } catch (std::exception const& error) {
            _failure = error.what();
        } catch (...) {
            _failure = "unknown exception";
        }
        _phase = Phase::Done;
        return true;
    }

    /// @brief Sets a receiver of (phases done, total) after each phase ends,
    ///        on the thread that runs the step.
    /// @param listener The receiver.
    void onPhase(std::function<void(std::size_t, std::size_t)> listener) { _onPhase = std::move(listener); }

    /// @brief Phases done and total, for a progress sink.
    /// @return (done, total).
    [[nodiscard]] std::pair<std::size_t, std::size_t> progress() const noexcept {
        return {static_cast<std::size_t>(_phase), static_cast<std::size_t>(Phase::Done)};
    }

    /// @brief Whether every phase has run.
    /// @return `true` when done.
    [[nodiscard]] bool done() const noexcept { return _phase == Phase::Done; }

    /// @brief Why the job failed: the message of the exception that ended it.
    /// @return The message, or nothing when the job did not fail.
    [[nodiscard]] std::optional<std::string> const& failure() const noexcept { return _failure; }

    /// @brief What the job reads.
    /// @return The input.
    [[nodiscard]] JobInput const& input() const noexcept { return _in; }

    /// @brief Moves the result out. Valid once `done()`; the input stays readable.
    /// @return The result.
    [[nodiscard]] JobResult takeResult() { return std::move(_result); }

private:
    enum class Phase : std::uint8_t { Keys, Order, Diff, Done };

    bool run(Deadline deadline, ::core::async::StopToken const& stop) {
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
            if (_onPhase) {
                auto const [doneUnits, total] = progress();
                _onPhase(doneUnits, total);
            }
            if (_phase != Phase::Done && detail::shouldYield(deadline, stop)) {
                return false;
            }
        }
        return true;
    }

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

    // Builds every needed column that has no keys, and patches the dirty rows
    // of every column that has them (on a copy: the old keys may still be
    // read by the owner or another job).
    bool buildKeys(Deadline deadline, ::core::async::StopToken const& stop) {
        MORPH_ZONE("table.keyBuild");
        auto const rows = _in.snapshot->rowCount();
        if (!_patched && !_in.dirty.empty()) {
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

    // Chooses the work: filter the cached sorted order (with the dirty rows
    // merged back into it); or filter, then sort the survivors; or sort every
    // row and cache the order.
    void plan() {
        auto const rows = _in.snapshot->rowCount();
        auto const filtering = _in.filter != nullptr && !_in.filter->empty();
        _started = true;
        if (_in.cachedSorted != nullptr) {
            if (_in.dirty.empty()) {
                _base = *_in.cachedSorted;
                _result.sorted = _in.cachedSorted;
            } else {
                _base = repairOrder(*_in.cachedSorted, _in.dirty, rowOrder());
                _result.sorted = std::make_shared<std::vector<std::uint32_t> const>(_base);
            }
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
        _result.view = std::move(_base);
        return true;
    }

    void diff() {
        MORPH_ZONE("table.apply");
        std::vector<std::uint32_t> changed = _in.dirty;
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
    // outcome
    std::optional<std::string> _failure;
    std::function<void(std::size_t, std::size_t)> _onPhase;
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
          _viewColumns{_columns},
          _viewSnapshot{_snapshot},
          _view(identity(_snapshot->rowCount())),
          _trueView{_view},
          _keysSnapshot{_snapshot},
          _keys(_columns.size()) {
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
    ///
    /// Keys are checked against the source's latest columns, which `columns()`
    /// shows once the view computed with them is applied.
    /// @param chain The keys, most significant first.
    /// @return `UnknownColumn` when a key names no column; `ServiceFailed` when
    ///         the sort ran inside the call and an injected service threw. The
    ///         sort is then unchanged.
    std::expected<void, TableError> setSort(SortChain chain) {
        for (auto const& key : chain) {
            if (!detail::findColumn(_columns, key.column)) {
                return std::unexpected(detail::unknownColumn(key.column));
            }
        }
        if (chain == _sort) {
            return {};
        }
        _sort = std::move(chain);
        return schedule();
    }

    /// @brief Sets the filter.
    /// @param spec The filter; an empty spec shows every row.
    /// @return The compile error, naming its column; or `ServiceFailed` when
    ///         the filter ran inside the call and an injected service threw.
    ///         The filter is then unchanged.
    std::expected<void, TableError> setFilter(FilterSpec spec) {
        auto compiled = compile(spec);
        if (!compiled) {
            return std::unexpected(std::move(compiled.error()));
        }
        _filterSpec = std::move(spec);
        _filter = std::make_shared<CompiledFilter const>(std::move(*compiled));
        return schedule();
    }

    /// @brief The current sort chain.
    /// @return The chain.
    [[nodiscard]] SortChain const& sort() const noexcept { return _sort; }

    /// @brief The current filter.
    /// @return The filter spec.
    [[nodiscard]] FilterSpec const& filter() const noexcept { return _filterSpec; }

    /// @brief The columns of the applied view: what `cellAt` indexes. A reset
    ///        that changes the columns changes them when its view is applied,
    ///        with a `Reset` change.
    /// @return The columns.
    [[nodiscard]] std::span<ColumnInfo const> columns() const noexcept { return _viewColumns; }

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
    /// @param column  Column index into `columns()`.
    /// @return The cell; empty for a column past `columns()`.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters) -- row then column, as every table reads them
    [[nodiscard]] Cell cellAt(std::size_t viewRow, std::size_t column) const {
        auto const row = _view.at(viewRow);
        if (column >= _viewColumns.size()) {
            return {};
        }
        return _viewSnapshot->cell(row, column);
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

    /// @brief Sets the receiver of errors no call could return: an injected
    ///        service that threw in a job that did not run inside the call
    ///        that asked for it. The sort and filter go back to those of the
    ///        view shown.
    /// @param handler The receiver; empty to stop receiving.
    void onError(std::function<void(TableError const&)> handler) { _onError = std::move(handler); }

    /// @brief Counters.
    /// @return The engine's counters so far.
    [[nodiscard]] EngineStats const& stats() const noexcept { return _stats; }

    /// @brief Marks a row as being edited: with `Deferred` reorder it keeps its
    ///        place through updates until `endEdit`.
    /// @param key The row's key.
    void beginEdit(RowId const& key) { _editing.insert(key); }

    /// @brief Ends an edit; a held row moves to its sorted place now.
    ///
    /// The commit that ends an edit usually patches the row just before, and
    /// that update reaches the engine on a later owner turn: the first job
    /// that includes it does not hold the row either, so it moves once rather
    /// than waiting for the settle interval.
    /// @param key The row's key.
    void endEdit(RowId const& key) {
        _editing.erase(key);
        if (_options.reorder == ReorderPolicy::Deferred) {
            _releaseOnRepair.insert_or_assign(key, _changeSeq);
        }
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

    // What the engine's state was when a job's input was taken, to tell on
    // the job's return whether a later change made its result useless.
    struct Ticket {
        std::uint64_t epoch = 0;           // structural changes absorbed
        std::uint64_t changeSeq = 0;       // notifications absorbed
        std::uint64_t columnsVersion = 0;  // column sets adopted
    };

    // What applying a result calls out with.
    struct Applied {
        ViewChange change;
        bool armSettle = false;
    };

    // ── changes ─────────────────────────────────────────────────────────────

    void rowsChanged(RowChange const& change) override {
        switch (change.kind) {
            case ChangeKind::Updated:
                for (auto const row : change.rows) {
                    _notifiedRows.push_back(static_cast<std::uint32_t>(row));
                }
                break;
            case ChangeKind::Reset:
                _notifiedReset = true;
                _notifiedStructural = true;
                break;
            case ChangeKind::Inserted:
            case ChangeKind::Removed:
            default:
                _notifiedStructural = true;
                break;
        }
        _notified = true;
        ++_changeSeq;
        if (_options.owner == nullptr) {
            flushChanges();
            return;
        }
        if (!_flushScheduled) {
            _flushScheduled = true;
            _options.owner->post(_alive.guard([this] { flushChanges(); }));
        }
    }

    // Takes in every change notified since the last flush, then asks for the
    // work it makes: one repair for any number of updates, or a recompute
    // after a structural change.
    void flushChanges() {
        MORPH_ZONE("table.snapshot");
        _flushScheduled = false;
        if (!absorb()) {
            return;
        }
        // An error comes back only from a job that ran inside this call, which
        // has then called nothing out.
        if (auto const done = schedule(); !done) {
            reportError(done.error());
        }
    }

    // Folds the notified changes into the pending work. Updated rows join the
    // dirty rows; a structural change drops the keys built for the old row
    // indices and owes every row a `Changed`.
    bool absorb() {
        if (!_notified) {
            return false;
        }
        _notified = false;
        _absorbedSeq = _changeSeq;
        _snapshot = _source->snapshot();
        if (_notifiedReset) {
            adoptColumns();
        }
        if (_notifiedStructural) {
            ++_epoch;
            _keys.assign(_columns.size(), nullptr);
            _keysSnapshot = nullptr;
            _sortCache = {};
            _dirty.clear();
            _obligeAll = true;
        } else {
            _dirty.insert(_dirty.end(), _notifiedRows.begin(), _notifiedRows.end());
        }
        _notifiedRows.clear();
        _notifiedStructural = false;
        _notifiedReset = false;
        return true;
    }

    // Re-reads the columns after a reset; a changed column set keeps the sort
    // keys and filter entries whose columns remain.
    void adoptColumns() {
        std::vector<ColumnInfo> columns(_source->columns().begin(), _source->columns().end());
        if (columns == _columns) {
            return;
        }
        _columns = std::move(columns);
        ++_columnsVersion;
        fitToColumns();
    }

    void fitToColumns() {
        std::erase_if(_sort, [&](SortKey const& key) { return !detail::findColumn(_columns, key.column); });
        std::erase_if(_filterSpec.columns,
                      [&](auto const& entry) { return !detail::findColumn(_columns, entry.first).has_value(); });
        for (auto& group : _filterSpec.groups) {
            std::erase_if(group.columns,
                          [&](std::string const& columnId) { return !detail::findColumn(_columns, columnId); });
        }
        std::erase_if(_filterSpec.groups, [](GroupFilter const& group) { return group.columns.empty(); });
        auto compiled = compile(_filterSpec);
        if (compiled) {
            _filter = std::make_shared<CompiledFilter const>(std::move(*compiled));
        } else {
            // A remaining entry no longer compiles for its column's new kind.
            _filterSpec = {};
            _filter = nullptr;
        }
    }

    // compileFilter, with an exception from the collator or date parser as an error.
    [[nodiscard]] std::expected<CompiledFilter, TableError> compile(FilterSpec const& spec) const {
        try {
            return compileFilter(spec, _columns, _options.services);
        } catch (std::exception const& error) {
            return std::unexpected(serviceFailed(error.what()));
        } catch (...) {
            return std::unexpected(serviceFailed("unknown exception"));
        }
    }

    [[nodiscard]] static TableError serviceFailed(std::string const& what) {
        return TableError{.code = TableErrorCode::ServiceFailed, .column = {}, .message = "a service failed: " + what};
    }

    // ── scheduling ──────────────────────────────────────────────────────────

    // Whether the shown view is out of date with the requested state.
    [[nodiscard]] bool hasWork() const {
        return !_dirty.empty() || _epoch != _viewEpoch || _sort != _appliedSort || _filter != _appliedFilter ||
               _columnsVersion != _viewColumnsVersion;
    }

    // A repair merges the dirty rows into the shown order: only the rows'
    // contents changed since that order was computed. (A structural change
    // advances the epoch; every row is owed a `Changed` exactly while the
    // epoch is ahead of the view's.)
    [[nodiscard]] bool canRepair() const {
        return !_dirty.empty() && _epoch == _viewEpoch && _sort == _appliedSort && _filter == _appliedFilter &&
               _columnsVersion == _viewColumnsVersion;
    }

    // Whether a change since @p input was taken makes its result useless.
    [[nodiscard]] bool supersedes(detail::JobInput const& input, Ticket const& ticket) const {
        return _sort != input.sort || _filter != input.filter || _epoch != ticket.epoch ||
               _columnsVersion != ticket.columnsVersion;
    }

    // Starts the work the requested state needs. While a job runs, a request
    // that makes its result useless stops it; either way the work follows it.
    std::expected<void, TableError> schedule() {
        if (_running) {
            if (supersedes(*_runningInput, _runningTicket)) {
                _jobStop.request_stop();
            }
            return {};
        }
        if (!hasWork()) {
            return {};
        }
        auto const kind = canRepair() ? Request::Repair : Request::Full;
        if (_options.owner == nullptr || _snapshot->rowCount() < _options.smallTable) {
            return runNow(kind);
        }
        start(kind);
        return {};
    }

    [[nodiscard]] detail::JobInput makeInput(Request kind, Ticket& ticket) {
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
        // The cached order is of the rows the keys were built from (install
        // keeps no other); the job merges the dirty rows back into it.
        if (!_sort.empty() && _sortCache.order != nullptr && _sortCache.chain == _sort) {
            input.cachedSorted = _sortCache.order;
        }
        input.previousView = _view;
        input.previousSnapshot = _viewSnapshot;
        input.repair = kind == Request::Repair;
        input.indicesStable = _epoch == _viewEpoch;
        input.dirty = std::move(_dirty);
        _dirty.clear();
        std::ranges::sort(input.dirty);
        auto const [dupFirst, dupLast] = std::ranges::unique(input.dirty);
        input.dirty.erase(dupFirst, dupLast);
        if (input.repair) {
            input.previousTrueView = _trueView;
        }
        input.allChanged = _obligeAll;
        _obligeAll = false;
        input.resetThreshold = _options.resetThreshold;
        ticket = Ticket{.epoch = _epoch, .changeSeq = _absorbedSeq, .columnsVersion = _columnsVersion};
        return input;
    }

    // A job that was dropped or failed hands back the work it took.
    void giveBack(detail::JobInput const& input, Ticket const& ticket) {
        if (ticket.epoch == _epoch) {
            _dirty.insert(_dirty.end(), input.dirty.begin(), input.dirty.end());
        }
        _obligeAll = _obligeAll || input.allChanged;
    }

    // A failed job's sort and filter go back to those of the view shown.
    void revert() {
        _sort = _appliedSort;
        _filterSpec = _appliedFilterSpec;
        _filter = _appliedFilter;
        if (_columnsVersion != _viewColumnsVersion) {
            fitToColumns();
        }
    }

    std::expected<void, TableError> runNow(Request kind) {
        auto const began = std::chrono::steady_clock::now();
        ++_stats.jobs;
        Ticket ticket;
        detail::ViewJob job{makeInput(kind, ticket)};
        static_cast<void>(job.step(kNoDeadline, ::core::async::StopToken{}));
        if (job.failure()) {
            recordOwnerStep(began);
            giveBack(job.input(), ticket);
            revert();
            return std::unexpected(serviceFailed(*job.failure()));
        }
        auto const applied = install(job.input(), job.takeResult(), ticket);
        recordOwnerStep(began);
        deliver(applied);
        return {};
    }

    void start(Request kind) {
        _running = true;
        ++_stats.jobs;
        _jobStop = ::core::async::StopSource{};
        auto const job = std::make_shared<detail::ViewJob>(makeInput(kind, _runningTicket));
        _runningInput = &job->input();
        auto* const owner = _options.owner;
        if (_options.worker == nullptr) {
            owner->post(_alive.guard([this, job] { ownerStep(job); }));
        } else {
            // The worker task never names the engine: it holds the job and
            // callbacks gated on the engine's lifetime, which run on the owner.
            if (auto const progress = _options.services.progress) {
                auto report =
                    _alive.guard([progress](std::size_t done, std::size_t total) { progress->progress(done, total); });
                job->onPhase([owner, report = std::move(report)](std::size_t done, std::size_t total) {
                    owner->post([report, done, total] mutable { report(done, total); });
                });
            }
            auto finished = _alive.guard([this, job] { finish(job); });
            _options.worker->post([job, stop = _jobStop.get_token(), owner, finished = std::move(finished)] mutable {
                static_cast<void>(job->step(kNoDeadline, stop));
                owner->post(std::move(finished));
            });
        }
        // Last: the handler may make a request or destroy the engine.
        setPending(true);
    }

    void ownerStep(std::shared_ptr<detail::ViewJob> const& job) {
        auto const began = std::chrono::steady_clock::now();
        auto const stop = _jobStop.get_token();
        bool const done = job->step(began + _options.frameBudget, stop);
        recordOwnerStep(began);
        if (auto const progress = _options.services.progress) {
            auto const alive = _alive.token();
            auto const [doneUnits, total] = job->progress();
            progress->progress(doneUnits, total);
            if (alive.expired()) {
                return;
            }
        }
        if (!done && !stop.stop_requested()) {
            _options.owner->post(_alive.guard([this, job] { ownerStep(job); }));
            return;
        }
        finish(job);
    }

    void finish(std::shared_ptr<detail::ViewJob> const& job) {
        _running = false;
        _runningInput = nullptr;
        auto const& input = job->input();
        if (!job->done() || supersedes(input, _runningTicket)) {
            ++_stats.dropped;
            giveBack(input, _runningTicket);
            afterJob(std::nullopt);
            return;
        }
        if (job->failure()) {
            giveBack(input, _runningTicket);
            revert();
            afterJob(serviceFailed(*job->failure()));
            return;
        }
        auto const began = std::chrono::steady_clock::now();
        auto const applied = install(input, job->takeResult(), _runningTicket);
        recordOwnerStep(began);
        if (!deliver(applied)) {
            return;
        }
        afterJob(std::nullopt);
    }

    // After a job returns: starts the work that remains (not after a failure,
    // which would only fail again), lays out holds released meanwhile, ends
    // `pending()`, and reports the failure. A handler may have started a job
    // already; then `pending()` stays true.
    void afterJob(std::optional<TableError> error) {
        auto const alive = _alive.token();
        if (!error && !_running && hasWork()) {
            auto const done = schedule();
            if (alive.expired()) {
                return;
            }
            if (!done) {
                error = done.error();
            }
        }
        if (_relayoutPending && !_running) {
            relayout();
            if (alive.expired()) {
                return;
            }
        }
        if (!_running) {
            setPending(false);
            if (alive.expired()) {
                return;
            }
        }
        if (error) {
            reportError(*error);
        }
    }

    // ── applying ────────────────────────────────────────────────────────────

    // Makes a finished job's result the shown view. Calls nothing out.
    [[nodiscard]] Applied install(detail::JobInput const& input, detail::JobResult result, Ticket const& ticket) {
        _stats.keyBuilds += result.keyBuilds;
        _stats.fullSorts += result.fullSort ? 1 : 0;
        _stats.subsetSorts += result.subsetSort ? 1 : 0;
        _stats.repairs += result.repaired ? 1 : 0;
        _keys = std::move(result.keys);
        _keysSnapshot = input.snapshot;
        if (result.sorted != nullptr) {
            _sortCache = SortCache{.chain = input.sort, .snapshot = input.snapshot, .order = std::move(result.sorted)};
        } else if (_sortCache.snapshot != input.snapshot) {
            // An order of older rows: a repair it missed would merge into it
            // as if it were current.
            _sortCache = {};
        }
        MORPH_PLOT("table.sourceRows", input.snapshot->rowCount());
        MORPH_PLOT("table.viewRows", result.view.size());
        MORPH_PLOT("table.keyCacheColumns",
                   std::ranges::count_if(_keys, [](auto const& keys) { return keys != nullptr; }));
        bool const columnsChanged = ticket.columnsVersion != _viewColumnsVersion;
        if (columnsChanged) {
            _viewColumns = input.columns;
            _viewColumnsVersion = ticket.columnsVersion;
        }
        _viewEpoch = ticket.epoch;
        _appliedSort = input.sort;
        _appliedFilter = input.filter;
        _appliedFilterSpec = _filterSpec;
        _trueView = std::move(result.view);
        _viewSnapshot = input.snapshot;
        _relayoutPending = false;
        _viewIndexValid = false;
        _keyIndexValid = false;
        Applied out{.change = std::move(result.change), .armSettle = false};
        if (_options.reorder == ReorderPolicy::Deferred) {
            // An ended edit's token stands for the notifications up to its own
            // count: one the shown view had already taken in has no use.
            std::erase_if(_releaseOnRepair, [&](auto const& entry) { return entry.second <= _viewChangeSeq; });
            if (input.repair) {
                holdUpdatedRows(input);
            } else {
                // A sort, filter or structural change releases every held row.
                _holds.clear();
            }
            std::erase_if(_releaseOnRepair, [&](auto const& entry) { return entry.second <= ticket.changeSeq; });
        }
        _viewChangeSeq = ticket.changeSeq;
        if (!_holds.empty()) {
            auto displayed = holdLayout(input.previousView);
            out.change = diffViews(input.previousView, displayed, input.dirty, input.snapshot->rowCount(),
                                   _options.resetThreshold);
            _view = std::move(displayed);
            out.armSettle = true;
        } else {
            _view = _trueView;
        }
        if (columnsChanged) {
            // Row operations cannot say that the columns changed.
            out.change.ops.assign(1, ViewOp{.kind = ViewOp::Kind::Reset, .first = 0, .count = 0, .to = 0});
        }
        return out;
    }

    // Under `Deferred`, every updated row that was shown, and every row being
    // edited, keeps its place; a row whose edit ended before its commit's
    // update arrived does not.
    void holdUpdatedRows(detail::JobInput const& input) {
        std::vector<bool> shown(input.snapshot->rowCount(), false);
        for (auto const row : input.previousView) {
            shown[row] = true;
        }
        for (auto const row : input.dirty) {
            auto key = input.snapshot->rowId(row);
            if (shown[row] && !_releaseOnRepair.contains(key)) {
                _holds.insert(std::move(key));
            }
        }
        for (auto const& editing : _editing) {
            _holds.insert(editing);
        }
    }

    // The true view with every held row put back at its displayed index.
    [[nodiscard]] std::vector<std::uint32_t> holdLayout(std::span<std::uint32_t const> displayed) const {
        std::vector<std::pair<std::size_t, std::uint32_t>> held;  // (displayed index, row)
        std::vector<bool> isHeld(_viewSnapshot->rowCount(), false);
        for (std::size_t i = 0; i < displayed.size(); ++i) {
            auto const row = displayed[i];
            if (_holds.contains(_viewSnapshot->rowId(row))) {
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

    // Lays the view out again after holds were released. While a job runs its
    // result is about to replace the view, and it was diffed against the view
    // as it is now: the release waits for it, and its layout takes it in.
    void relayout() {
        if (_running) {
            _relayoutPending = true;
            return;
        }
        _relayoutPending = false;
        auto displayed = _holds.empty() ? _trueView : holdLayout(_view);
        auto const change = diffViews(_view, displayed, {}, _viewSnapshot->rowCount(), _options.resetThreshold);
        _view = std::move(displayed);
        _viewIndexValid = false;
        _keyIndexValid = false;
        deliver(Applied{.change = change, .armSettle = false});
    }

    // ── calling out ─────────────────────────────────────────────────────────

    // Arms the settle timer and publishes the change. Returns `false` when a
    // handler destroyed the engine; the caller then returns at once.
    bool deliver(Applied const& applied) {
        auto const alive = _alive.token();
        if (applied.armSettle && _options.scheduleSettle) {
            auto const seq = _changeSeq;
            auto const scheduler = _options.scheduleSettle;
            scheduler(_options.settle, _alive.guard([this, seq] {
                if (seq == _changeSeq) {
                    settleNow();
                }
            }));
            if (alive.expired()) {
                return false;
            }
        }
        if (!applied.change.empty() && _onViewChange) {
            // A copy: the handler may replace itself or destroy the engine.
            auto const handler = _onViewChange;
            handler(applied.change);
        }
        return !alive.expired();
    }

    void setPending(bool value) {
        if (_pending == value) {
            return;
        }
        _pending = value;
        if (_onPending) {
            auto const handler = _onPending;
            handler(value);
        }
    }

    void reportError(TableError const& error) const {
        if (_onError) {
            auto const handler = _onError;
            handler(error);
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
    // requested state: what the next job computes
    std::vector<ColumnInfo> _columns;
    std::shared_ptr<RowSnapshot const> _snapshot;
    SortChain _sort;
    FilterSpec _filterSpec;
    std::shared_ptr<CompiledFilter const> _filter;
    std::uint64_t _columnsVersion = 0;
    std::uint64_t _epoch = 0;
    // applied state: what the shown view was computed from
    std::vector<ColumnInfo> _viewColumns;
    std::shared_ptr<RowSnapshot const> _viewSnapshot;
    SortChain _appliedSort;
    FilterSpec _appliedFilterSpec;
    std::shared_ptr<CompiledFilter const> _appliedFilter;
    std::uint64_t _viewColumnsVersion = 0;
    std::uint64_t _viewEpoch = 0;
    std::uint64_t _viewChangeSeq = 0;      // notifications the view took in
    std::vector<std::uint32_t> _view;      // displayed
    std::vector<std::uint32_t> _trueView;  // sorted and filtered, without holds
    // caches, valid for `_keysSnapshot`
    std::shared_ptr<RowSnapshot const> _keysSnapshot;
    std::vector<std::shared_ptr<KeyColumn const>> _keys;
    SortCache _sortCache;
    mutable std::vector<std::uint32_t> _viewIndex;
    mutable bool _viewIndexValid = false;
    mutable std::unordered_map<RowId, std::size_t> _keyIndex;
    mutable bool _keyIndexValid = false;
    // pending work
    std::vector<std::uint32_t> _notifiedRows;  // updated, not yet absorbed
    bool _notified = false;
    bool _notifiedStructural = false;
    bool _notifiedReset = false;
    bool _flushScheduled = false;
    std::uint64_t _changeSeq = 0;       // notifications received
    std::uint64_t _absorbedSeq = 0;     // notifications absorbed
    std::vector<std::uint32_t> _dirty;  // rows changed since `_keysSnapshot`
    bool _obligeAll = false;            // every row is owed a `Changed`
    // deferred reorder
    std::unordered_set<RowId> _holds;
    std::unordered_set<RowId> _editing;
    std::unordered_map<RowId, std::uint64_t> _releaseOnRepair;  // ended edits, by the notification count then
    bool _relayoutPending = false;
    // execution
    bool _pending = false;
    bool _running = false;
    detail::JobInput const* _runningInput = nullptr;
    Ticket _runningTicket;
    ::core::async::StopSource _jobStop;
    std::function<void(ViewChange const&)> _onViewChange;
    std::function<void(bool)> _onPending;
    std::function<void(TableError const&)> _onError;
    EngineStats _stats;
    async::CallbackScope _alive;
};

// NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)

}  // namespace morph::table

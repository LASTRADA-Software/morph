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
#include <iterator>
#include <limits>
#include <map>
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
#include <string_view>
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
    /// @brief Filter runs over a candidate list; a repair evaluates its dirty rows alone and runs none.
    std::size_t filterRuns = 0;
    /// @brief Results dropped because a newer request superseded them.
    std::size_t dropped = 0;
    /// @brief Steps run on the owner thread (one-step runs included).
    std::size_t ownerSteps = 0;
    /// @brief The longest single step on the owner thread, in microseconds.
    std::size_t maxOwnerStepMicros = 0;
};

/// @brief A column layout as it is stored: `{"order", "hidden", "pinned", "widths", "groups", "units"}`.
///
/// Read with glaze's reflection; a member the stored text leaves out reads as its default.
struct ColumnLayoutValue {
    /// @brief The columns in display order.
    std::vector<std::string> order;
    /// @brief The hidden columns, in display order.
    std::vector<std::string> hidden;
    /// @brief How many of the first visible columns are pinned.
    std::size_t pinned = 0;
    /// @brief Column widths in units, by column id; every width is above zero.
    std::map<std::string, int> widths;
    /// @brief The groups the user formed.
    std::vector<ColumnGroupInfo> groups;
    /// @brief Display units: by column id, a unit id per dimension.
    std::map<std::string, std::map<std::string, std::string>> units;

    /// @brief Member-wise equality.
    /// @param other The layout to compare with.
    /// @return `true` when every member matches.
    [[nodiscard]] bool operator==(ColumnLayoutValue const& other) const = default;
};

/// @brief Column order, visibility, widths and pinning, kept apart from the engine's view.
///
/// At least one column stays visible. A stored layout is fitted to the table's declared columns (`apply`): a
/// column the table no longer has is dropped from every part of it, and a column the layout does not name takes its
/// declared visibility and its declared place, right after the nearest column declared before it that the layout
/// places, or first when there is none. A user-formed group keeps the columns no earlier group holds; one left with
/// fewer than 2 or more than 16 columns, or repeating an earlier group's id, is dropped.
class ColumnLayout {
public:
    /// @brief A layout of @p ids in that order, @p declaredHidden hidden.
    /// @param ids            Column ids, the declared order.
    /// @param declaredHidden The columns hidden until the user shows them.
    explicit ColumnLayout(std::vector<std::string> ids, std::vector<std::string> declaredHidden = {})
        : _defaults{std::move(ids)}, _declaredHidden{std::move(declaredHidden)} {
        reset();
    }

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

    /// @brief Shows or hides a column; the pinned count follows the visible columns.
    /// @param columnId The column.
    /// @param show     `true` to show.
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
        clampPinned();
        return true;
    }

    /// @brief Moves a column to a display position.
    /// @param columnId The column.
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

    /// @brief Sets or clears a column's width.
    /// @param columnId The column.
    /// @param width    The width in units, above zero; nothing clears it.
    /// @return `false` when the column is unknown or the width is not above zero.
    bool setWidth(std::string const& columnId, std::optional<int> width) {
        if (std::ranges::find(_order, columnId) == _order.end() || (width && *width <= 0)) {
            return false;
        }
        if (width) {
            _widths[columnId] = *width;
        } else {
            _widths.erase(columnId);
        }
        return true;
    }

    /// @brief The widths set, by column id.
    /// @return The widths.
    [[nodiscard]] std::map<std::string, int> const& widths() const noexcept { return _widths; }

    /// @brief Pins the first @p n visible columns.
    /// @param n The count; clamped to the visible columns.
    void setPinned(std::size_t n) {
        _pinned = n;
        clampPinned();
    }

    /// @brief How many of the first columns of `visible()` are pinned.
    /// @return The count.
    [[nodiscard]] std::size_t pinned() const noexcept { return _pinned; }

    /// @brief Forms a group of columns under a label, as the user does in the column chooser.
    /// @param label   The group's label.
    /// @param columns The columns, 2 to 16, each a column of the layout and in no other user-formed group.
    /// @return The new group's id, the first of `g1`, `g2`, … no group has; `InvalidSpec` when @p columns breaks the
    ///         rule above or repeats a column.
    std::expected<std::string, TableError> formGroup(std::string label, std::vector<std::string> columns) {
        auto const refuse = [](std::string message) {
            return std::unexpected(
                TableError{.code = TableErrorCode::InvalidSpec, .column = {}, .message = std::move(message)});
        };
        if (columns.size() < 2 || columns.size() > kMaxUserGroupColumns) {
            return refuse("a group has 2 to 16 columns, not " + std::to_string(columns.size()));
        }
        for (std::size_t at = 0; at < columns.size(); ++at) {
            auto const& columnId = columns[at];
            if (std::ranges::find(_order, columnId) == _order.end()) {
                return refuse("unknown column: " + columnId);
            }
            if (std::ranges::find(columns.begin(), columns.begin() + static_cast<std::ptrdiff_t>(at), columnId) !=
                columns.begin() + static_cast<std::ptrdiff_t>(at)) {
                return refuse("column " + columnId + " is named twice");
            }
            for (auto const& group : _groups) {
                if (std::ranges::find(group.columns, columnId) != group.columns.end()) {
                    return refuse("column " + columnId + " is already in group " + group.label);
                }
            }
        }
        std::string groupId;
        for (std::size_t number = 1;; ++number) {
            groupId = "g" + std::to_string(number);
            if (std::ranges::none_of(_groups, [&](ColumnGroupInfo const& group) { return group.id == groupId; })) {
                break;
            }
        }
        _groups.push_back({.id = groupId, .label = std::move(label), .columns = std::move(columns)});
        return groupId;
    }

    /// @brief Removes a user-formed group.
    /// @param groupId The group's id.
    /// @return `false` when no group has that id.
    bool removeGroup(std::string const& groupId) {
        return std::erase_if(_groups, [&](ColumnGroupInfo const& group) { return group.id == groupId; }) != 0;
    }

    /// @brief The groups the user formed, in the order they were formed.
    /// @return The groups.
    [[nodiscard]] std::span<ColumnGroupInfo const> groups() const noexcept { return _groups; }

    /// @brief Sets or clears the unit a column shows one of its dimensions in.
    /// @param columnId  The column.
    /// @param dimension The dimension, such as `mass`.
    /// @param unit      The unit's id; `nullopt` clears it, and the cells show their own units.
    /// @return `false` when the column is unknown, or @p dimension or @p unit is empty.
    bool setUnit(std::string const& columnId, std::string const& dimension, std::optional<std::string> unit) {
        if (std::ranges::find(_order, columnId) == _order.end() || dimension.empty() || (unit && unit->empty())) {
            return false;
        }
        if (unit) {
            _units[columnId][dimension] = std::move(*unit);
            return true;
        }
        if (auto const found = _units.find(columnId); found != _units.end()) {
            found->second.erase(dimension);
            if (found->second.empty()) {
                _units.erase(found);
            }
        }
        return true;
    }

    /// @brief The display units: by column id, a unit id per dimension.
    /// @return The units.
    [[nodiscard]] std::map<std::string, std::map<std::string, std::string>> const& units() const noexcept {
        return _units;
    }

    /// @brief The layout as it is stored.
    /// @return Order, hidden columns in display order, pinned count, widths, groups and units.
    [[nodiscard]] ColumnLayoutValue value() const {
        ColumnLayoutValue out;
        out.order = _order;
        for (auto const& columnId : _order) {
            if (_hidden.contains(columnId)) {
                out.hidden.push_back(columnId);
            }
        }
        out.pinned = _pinned;
        out.widths = _widths;
        out.groups = _groups;
        out.units = _units;
        return out;
    }

    /// @brief Adopts a stored layout, fitted to the declared columns (see the class).
    /// @param value The stored layout.
    void apply(ColumnLayoutValue const& value) { fit(value); }

    /// @brief Restores the declared order and hidden columns, and clears widths and pinning; groups and units stay.
    void reset() {
        _order = _defaults;
        _hidden.clear();
        for (auto const& columnId : _declaredHidden) {
            if (std::ranges::find(_order, columnId) != _order.end()) {
                _hidden.insert(columnId);
            }
        }
        if (!_order.empty() && visible().empty()) {
            _hidden.erase(_order.front());
        }
        _widths.clear();
        _pinned = 0;
    }

    /// @brief Adopts a new declared column list and fits the current layout to it.
    /// @param ids            The new columns, in their declared order.
    /// @param declaredHidden The new declared hidden columns.
    void rebuild(std::vector<std::string> ids, std::vector<std::string> declaredHidden = {}) {
        auto const current = value();
        _defaults = std::move(ids);
        _declaredHidden = std::move(declaredHidden);
        fit(current);
    }

private:
    // The fitting rule: every part of `value` filtered to the declared columns, each declared column it lacks placed
    // after its nearest declared predecessor that is placed, one column kept visible, and the pin count clamped.
    // NOLINTNEXTLINE(readability-function-cognitive-complexity): one sequential pass whose steps share its running state; splitting it would thread that state through parameters.
    void fit(ColumnLayoutValue const& value) {
        // Sets, not scans: a stored layout of a few hundred columns is fitted in one pass over each of its parts.
        std::unordered_set<std::string_view> const declaredIds{_defaults.begin(), _defaults.end()};
        auto const declared = [&declaredIds](std::string const& columnId) { return declaredIds.contains(columnId); };
        std::vector<std::string> order;
        std::unordered_set<std::string_view> placed;
        order.reserve(_defaults.size());
        for (auto const& columnId : value.order) {
            if (declared(columnId) && placed.insert(columnId).second) {
                order.push_back(columnId);
            }
        }
        std::vector<std::string> unmentioned;
        for (std::size_t at = 0; at < _defaults.size(); ++at) {
            auto const& columnId = _defaults[at];
            if (placed.contains(columnId)) {
                continue;
            }
            auto place = order.begin();
            for (std::size_t before = at; before-- > 0;) {
                if (auto const found = std::ranges::find(order, _defaults[before]); found != order.end()) {
                    place = std::next(found);
                    break;
                }
            }
            order.insert(place, columnId);
            unmentioned.push_back(columnId);
        }
        _order = std::move(order);
        _hidden.clear();
        for (auto const& columnId : value.hidden) {
            if (declared(columnId)) {
                _hidden.insert(columnId);
            }
        }
        // A column the stored layout predates keeps its author's visibility, so a column declared hidden is not
        // shown, nor its level fetched, for every user who has a layout.
        for (auto const& columnId : unmentioned) {
            if (std::ranges::find(_declaredHidden, columnId) != _declaredHidden.end()) {
                _hidden.insert(columnId);
            }
        }
        _widths.clear();
        for (auto const& [columnId, width] : value.widths) {
            if (declared(columnId) && width > 0) {
                _widths.emplace(columnId, width);
            }
        }
        _groups.clear();
        std::unordered_set<std::string> grouped;
        for (auto group : value.groups) {
            if (std::ranges::any_of(_groups, [&](ColumnGroupInfo const& kept) { return kept.id == group.id; })) {
                continue;
            }
            std::vector<std::string> columns;
            for (auto& columnId : group.columns) {
                if (declared(columnId) && !grouped.contains(columnId) &&
                    std::ranges::find(columns, columnId) == columns.end()) {
                    columns.push_back(std::move(columnId));
                }
            }
            if (columns.size() >= 2 && columns.size() <= kMaxUserGroupColumns) {
                grouped.insert(columns.begin(), columns.end());
                group.columns = std::move(columns);
                _groups.push_back(std::move(group));
            }
        }
        _units.clear();
        for (auto const& [columnId, units] : value.units) {
            if (declared(columnId)) {
                _units.emplace(columnId, units);
            }
        }
        if (!_defaults.empty() && visible().empty()) {
            _hidden.erase(_defaults.front());
        }
        _pinned = value.pinned;
        clampPinned();
    }

    void clampPinned() { _pinned = std::min(_pinned, visible().size()); }

    // A group's column bound, `QueryLimits::maxGroupColumns`, which `query.hpp` holds and this header cannot include.
    static constexpr std::size_t kMaxUserGroupColumns = 16;

    std::vector<std::string> _defaults;
    std::vector<std::string> _declaredHidden;
    std::vector<std::string> _order;
    std::unordered_set<std::string> _hidden;
    std::map<std::string, int> _widths;
    std::size_t _pinned = 0;
    std::vector<ColumnGroupInfo> _groups;
    std::map<std::string, std::map<std::string, std::string>> _units;
};

namespace detail {

/// @brief A view with retained rows merged in, as `mergeRetainedOrder` returns it.
struct RetainedOrder {
    /// @brief Source rows, in view order: the base order with the admitted rows at their sorted places.
    std::vector<std::uint32_t> order;
    /// @brief The view rows of the admitted rows whose match is `No`, ascending.
    std::vector<std::int64_t> unmatched;
    /// @brief The retained rows the base order lacked, which the merge admitted, ascending by source row.
    std::vector<std::uint32_t> admitted;
    /// @brief How many of `admitted` the filter could not decide (`Match::Unknown`).
    std::int64_t undecided = 0;
};

/// @brief Merges the retained rows a base order lacks into it, each at the place the sort gives it.
///
/// The base order must be sorted by @p before, which is a strict total order (ties to the lower source row, as
/// `RowOrder` breaks them), so the result is the order the sort gives the base rows and the admitted rows together.
/// Every admitted row is in the result whatever its match. Costs the scan of @p baseOrder for the retained rows it
/// holds, and a binary search of it for each admitted row.
/// @param baseOrder    The view without retained rows: source rows, sorted by @p before.
/// @param retainedRows The retained rows, as source rows; a row named twice is admitted once.
/// @param before       Whether one source row orders before another.
/// @param match        What the filter says of a source row.
/// @return The merged order, the unmatched view rows, the admitted rows and how many of those were undecided.
[[nodiscard]] inline RetainedOrder mergeRetainedOrder(std::span<std::uint32_t const> baseOrder,
                                                      std::span<std::uint32_t const> retainedRows,
                                                      std::function<bool(std::uint32_t, std::uint32_t)> const& before,
                                                      std::function<Match(std::uint32_t)> const& match) {
    RetainedOrder out;
    std::unordered_set<std::uint32_t> lacking(retainedRows.begin(), retainedRows.end());
    for (auto const row : baseOrder) {
        if (lacking.empty()) {
            break;
        }
        lacking.erase(row);
    }
    out.admitted.assign(lacking.begin(), lacking.end());
    std::ranges::sort(out.admitted);
    std::vector<std::pair<std::uint32_t, Match>> placed;
    placed.reserve(out.admitted.size());
    for (auto const row : out.admitted) {
        auto const decided = match(row);
        if (decided == Match::Unknown) {
            ++out.undecided;
        }
        placed.emplace_back(row, decided);
    }
    std::ranges::sort(placed,
                      [&before](auto const& left, auto const& right) { return before(left.first, right.first); });
    out.order.reserve(baseOrder.size() + placed.size());
    auto from = baseOrder.begin();
    for (auto const& [row, decided] : placed) {
        auto const last =
            std::upper_bound(from, baseOrder.end(), row,
                             [&before](std::uint32_t left, std::uint32_t right) { return before(left, right); });
        out.order.insert(out.order.end(), from, last);
        from = last;
        if (decided == Match::No) {
            out.unmatched.push_back(static_cast<std::int64_t>(out.order.size()));
        }
        out.order.push_back(row);
    }
    out.order.insert(out.order.end(), from, baseOrder.end());
    return out;
}

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
    /// @brief Repair: the rows the filter could not decide, source rows ascending. A dirty row may have been here
    ///        or in the view, so a repair reads both.
    std::vector<std::uint32_t> previousUndecided;
    /// @brief The job repairs updated rows rather than recomputing.
    bool repair = false;
    /// @brief Every surviving row may have new content (a reset).
    bool allChanged = false;
    /// @brief More operations than this make the change a reset.
    std::size_t resetThreshold = 5000;
    /// @brief Keys of rows the view keeps whatever the filter says of them; a key the snapshot lacks is ignored.
    std::vector<RowId> retained;
};

/// @brief What a finished job hands back to the owner.
struct JobResult {
    /// @brief Key columns by column index, for `JobInput::snapshot`.
    std::vector<std::shared_ptr<KeyColumn const>> keys;
    /// @brief A full order for the chain, to cache, or null.
    std::shared_ptr<std::vector<std::uint32_t> const> sorted;
    /// @brief The sorted, filtered view.
    std::vector<std::uint32_t> view;
    /// @brief The rows the filter could neither admit nor reject, source rows ascending. They are in no view.
    std::vector<std::uint32_t> undecided;
    /// @brief From `JobInput::previousView` to `view`.
    ViewChange change;
    /// @brief Every source row was sorted.
    bool fullSort = false;
    /// @brief Only the filter's survivors were sorted.
    bool subsetSort = false;
    /// @brief Updated rows were merged into the previous order.
    bool repaired = false;
    /// @brief A filter ran over a candidate list.
    bool filtered = false;
    /// @brief Key columns built from scratch.
    std::size_t keyBuilds = 0;
    /// @brief The retained rows in `view` the filter rejects, source rows ascending.
    std::vector<std::uint32_t> unmatched;
};

/// @brief The error of an unloaded cell in a base-level column: no level names that cell, so no load would
///        resolve it, and a view or a values list built on it would never settle.
/// @param info The column.
/// @return A `SourceContract` error naming the column.
[[nodiscard]] inline TableError unloadedInBaseLevel(ColumnInfo const& info) {
    return TableError{.code = TableErrorCode::SourceContract,
                      .column = info.id,
                      .message = "an unloaded cell in a base-level column"};
}

/// @brief The error of a job an injected service failed.
/// @param what The message of the exception the service threw.
/// @return A `ServiceFailed` error.
[[nodiscard]] inline TableError serviceFailed(std::string const& what) {
    return TableError{.code = TableErrorCode::ServiceFailed, .column = {}, .message = "a service failed: " + what};
}

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
    /// comparator) ends the job: it is done, with `failure()` set to
    /// `ServiceFailed`. So does an unloaded cell in a base-level column, with
    /// `SourceContract` naming the column.
    /// @param deadline When to yield.
    /// @param stop     A stop request ends the step.
    /// @return `true` when the job is done.
    bool step(Deadline deadline, ::core::async::StopToken const& stop) {
        try {
            return run(deadline, stop);
        } catch (std::exception const& error) {
            _failure = serviceFailed(error.what());
        } catch (...) {
            _failure = serviceFailed("unknown exception");
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

    /// @brief Why the job failed.
    /// @return The error that ended it, or nothing when the job did not fail.
    [[nodiscard]] std::optional<TableError> const& failure() const noexcept { return _failure; }

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
            if (_failure) {
                _phase = Phase::Done;
                return true;
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

    // Fails the job when a base-level column holds an unloaded cell (`detail::unloadedInBaseLevel`).
    bool refuseUnloaded(std::size_t column, CellState state) {
        if (state != CellState::Unloaded || !_in.columns[column].level.empty()) {
            return false;
        }
        _failure = unloadedInBaseLevel(_in.columns[column]);
        return true;
    }

    // Builds every needed column that has no keys, and patches the dirty rows
    // of every column that has them (on a copy: the old keys may still be
    // read by the owner or another job).
    // NOLINTNEXTLINE(readability-function-cognitive-complexity): one sequential pass whose steps share its running state; splitting it would thread that state through parameters.
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
                    if (refuseUnloaded(column, copy->states[row])) {
                        return true;
                    }
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
            if (_in.columns[column].level.empty()) {
                if (auto const found = std::ranges::find(_building->states, CellState::Unloaded);
                    found != _building->states.end() && refuseUnloaded(column, *found)) {
                    return true;
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
    // full order), without sorting the rest. Each dirty row is evaluated
    // again, wherever it was before: admitted rows merge into the view,
    // undecided ones join the undecided rows, rejected ones join neither. The
    // clean undecided rows are kept as they were, with no filter run over them.
    void repair() {
        MORPH_ZONE("table.change");
        auto const orderBy = rowOrder();
        auto const keys = keyPointers();
        std::vector<std::uint32_t> passing;
        std::vector<std::uint32_t> undecided;
        for (auto const row : _in.dirty) {
            auto const match = _in.filter == nullptr ? Match::Yes : _in.filter->evaluateRow(keys, row);
            if (match == Match::Yes) {
                passing.push_back(row);
            } else if (match == Match::Unknown) {
                undecided.push_back(row);
            }
        }
        std::vector<std::uint32_t> clean;
        std::ranges::set_difference(_in.previousUndecided, _in.dirty, std::back_inserter(clean));
        _result.undecided.resize(clean.size() + undecided.size());
        std::ranges::merge(clean, undecided, _result.undecided.begin());
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
            // NOLINTNEXTLINE(modernize-use-ranges): std::ranges::iota is missing from Emscripten's libc++.
            std::iota(_base.begin(), _base.end(), 0U);
            _sorted = _in.sort.empty();
            if (!_sorted && !filtering) {
                _sort.emplace(std::move(_base), rowOrder());
                _result.fullSort = true;
            }
        }
        if (filtering && (_sorted || _sort == std::nullopt)) {
            _filterRun.emplace(*_in.filter, keyPointers(), std::move(_base));
            _result.filtered = true;
        }
    }

    bool order(Deadline deadline, ::core::async::StopToken const& stop) {
        if (_in.repair) {
            repair();
            admitRetained();
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
            _result.undecided = std::move(*_filterRun).takeUndecided();
            std::ranges::sort(_result.undecided);
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
        admitRetained();
        return true;
    }

    // Merges the retained rows the filter left out into the view at their sorted places; those it could not decide
    // leave the undecided rows, and those it rejects are listed in `unmatched`.
    void admitRetained() {
        if (_in.retained.empty()) {
            return;
        }
        std::unordered_set<RowId> const wanted(_in.retained.begin(), _in.retained.end());
        std::vector<std::uint32_t> rows;
        for (std::size_t row = 0; row < _in.snapshot->rowCount() && rows.size() < wanted.size(); ++row) {
            if (wanted.contains(_in.snapshot->rowId(row))) {
                rows.push_back(static_cast<std::uint32_t>(row));
            }
        }
        auto const orderBy = rowOrder();
        auto const keys = keyPointers();
        auto merged = mergeRetainedOrder(
            _result.view, rows, [&orderBy](std::uint32_t left, std::uint32_t right) { return orderBy(left, right); },
            [this, &keys](std::uint32_t row) {
                return _in.filter == nullptr ? Match::Yes : _in.filter->evaluateRow(keys, row);
            });
        _result.view = std::move(merged.order);
        std::vector<std::uint32_t> undecided;
        std::ranges::set_difference(_result.undecided, merged.admitted, std::back_inserter(undecided));
        _result.undecided = std::move(undecided);
        _result.unmatched.clear();
        for (auto const column : merged.unmatched) {
            _result.unmatched.push_back(_result.view[static_cast<std::size_t>(column)]);
        }
        std::ranges::sort(_result.unmatched);
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
    std::optional<TableError> _failure;
    std::function<void(std::size_t, std::size_t)> _onPhase;
};

}  // namespace detail

struct DistinctRequest;
struct DistinctValues;

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
    ///         the sort ran inside the call and an injected service threw;
    ///         `SourceContract` when it ran inside the call and a base-level
    ///         column holds an unloaded cell, or when a key's column is `Measure` and the services have no unit
    ///         registry. The sort is then unchanged.
    std::expected<void, TableError> setSort(SortChain chain) {
        for (auto const& key : chain) {
            auto const index = detail::findColumn(_columns, key.column);
            if (!index) {
                return std::unexpected(detail::unknownColumn(key.column));
            }
            if (auto missing = detail::unitsMissing(_columns[*index], _options.services)) {
                return std::unexpected(std::move(*missing));
            }
        }
        if (chain == _sort) {
            return {};
        }
        _sort = std::move(chain);
        return schedule();
    }

    /// @brief Sets the filter.
    ///
    /// A group filter's `group` resolves through the source's declared groups, which are checked first
    /// (`detail::checkGroups`).
    /// @param spec The filter; an empty spec shows every row.
    /// @return `SourceContract` when the source's declared groups are malformed; the compile error, naming its
    ///         column; or `ServiceFailed` or `SourceContract`, as for `setSort`, when the filter ran inside the
    ///         call. The filter is then unchanged.
    std::expected<void, TableError> setFilter(FilterSpec spec) {
        if (auto error = detail::checkGroups(_columns, _source->groups())) {
            return std::unexpected(std::move(*error));
        }
        auto compiled = compile(spec);
        if (!compiled) {
            return std::unexpected(std::move(compiled.error()));
        }
        _filterSpec = std::move(spec);
        _filter = std::make_shared<CompiledFilter const>(std::move(*compiled));
        // The retained rows are of the filter they no longer matched.
        if (!_retained.empty()) {
            _retained.clear();
            ++_retainedVersion;
        }
        return schedule();
    }

    /// @brief Replaces the retained rows: rows the view keeps, at their sorted places, whatever the filter says of
    ///        them. `setFilter` clears them.
    /// @param keys The rows' keys; a key the source lacks is ignored.
    /// @return `ServiceFailed` or `SourceContract`, as for `setSort`, when the view was computed inside the call; the
    ///         retained rows are then those of the view shown.
    std::expected<void, TableError> retain(std::vector<RowId> keys) {
        if (keys == _retained) {
            return {};
        }
        _retained = std::move(keys);
        ++_retainedVersion;
        return schedule();
    }

    /// @brief The retained rows asked for.
    /// @return Their keys, as `retain` last set them.
    [[nodiscard]] std::span<RowId const> retained() const noexcept { return _retained; }

    /// @brief Whether the row at a view row is retained and rejected by the filter.
    /// @param viewRow View row index.
    /// @return `true` for such a row; `false` past the end.
    [[nodiscard]] bool isUnmatched(std::size_t viewRow) const {
        auto const row = sourceRowOf(viewRow);
        return row && std::ranges::binary_search(_unmatched, static_cast<std::uint32_t>(*row));
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

    /// @brief Rows the filter can neither admit nor reject until an unloaded
    ///        cell loads. They are in no view row.
    /// @return The count.
    [[nodiscard]] std::size_t undecidedRowCount() const noexcept { return _undecided.size(); }

    /// @brief The undecided rows.
    /// @return Source rows of `viewSnapshot()`, ascending.
    [[nodiscard]] std::span<std::uint32_t const> undecidedRows() const noexcept { return _undecided; }

    /// @brief Whether a computation is in flight.
    /// @return `true` from an asynchronous request until its result is applied.
    [[nodiscard]] bool pending() const noexcept { return _pending; }

    /// @brief Sets the receiver of view changes, called on the owner after the
    ///        view changed.
    /// @param handler The receiver; empty to stop receiving.
    void onViewChange(std::function<void(ViewChange const&)> handler) { _onViewChange = std::move(handler); }

    /// @brief Sets the receiver of `undecidedRowCount()` changes, called on the
    ///        owner with the new count: after `onViewChange` when the same
    ///        result changed the view, and on its own when it changed nothing else.
    /// @param handler The receiver; empty to stop receiving.
    void onUndecidedChange(std::function<void(std::size_t)> handler) { _onUndecidedChange = std::move(handler); }

    /// @brief Sets the receiver of `pending()` changes.
    /// @param handler The receiver; empty to stop receiving.
    void onPending(std::function<void(bool)> handler) { _onPending = std::move(handler); }

    /// @brief Sets the receiver of errors no call could return: an injected
    ///        service that threw, or a source that broke its contract, in a job
    ///        that did not run inside the call that asked for it. The sort and
    ///        filter go back to those of the view shown.
    /// @param handler The receiver; empty to stop receiving.
    void onError(std::function<void(TableError const&)> handler) { _onError = std::move(handler); }

    /// @brief Counters.
    /// @return The engine's counters so far.
    [[nodiscard]] EngineStats const& stats() const noexcept { return _stats; }

    /// @brief The distinct values of a column over the rows the engine's other filters leave: client mode's
    ///        `distinctValues`.
    ///
    /// Runs on the calling thread over the engine's latest snapshot, with the filter `filter()` returns less the
    /// column's own entry. The key columns the engine holds for that snapshot are read; one it does not hold is built
    /// for the call and not kept. The request is checked as `validate` checks it, with default `QueryLimits` and
    /// every column readable. Defined in `table/query.hpp`, which declares the request and the reply.
    /// @param request The column, search, bucket and page of values.
    /// @return The values, or the request's error.
    [[nodiscard]] std::expected<DistinctValues, TableError> distinctValues(DistinctRequest const& request) const;

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

    /// @brief Takes in every change the source notified at once, rather than at the owner's next turn: what a
    ///        caller that changed the source and reads the view in the same turn calls. Nothing notified, nothing
    ///        done.
    void flush() { flushChanges(); }

    /// @brief Rows currently held in place by `Deferred` reorder.
    /// @return Their keys.
    [[nodiscard]] std::vector<RowId> heldRows() const { return {_holds.begin(), _holds.end()}; }

private:
    static constexpr auto kAbsent = std::numeric_limits<std::uint32_t>::max();

    [[nodiscard]] static std::vector<std::uint32_t> identity(std::size_t rows) {
        std::vector<std::uint32_t> out(rows);
        // NOLINTNEXTLINE(modernize-use-ranges): std::ranges::iota is missing from Emscripten's libc++.
        std::iota(out.begin(), out.end(), 0U);
        return out;
    }

    enum class Request : std::uint8_t { Full, Repair };

    // What the engine's state was when a job's input was taken, to tell on
    // the job's return whether a later change made its result useless.
    struct Ticket {
        std::uint64_t epoch = 0;            // structural changes absorbed
        std::uint64_t changeSeq = 0;        // notifications absorbed
        std::uint64_t columnsVersion = 0;   // column sets adopted
        std::uint64_t retainedVersion = 0;  // retained sets asked for
    };

    // What applying a result calls out with.
    struct Applied {
        ViewChange change;
        bool armSettle = false;
        std::optional<std::size_t> undecided;  // the new count, when it changed
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
        // A filter on a declared group stays while the source still declares the group.
        auto const declared = _source->groups();
        std::erase_if(_filterSpec.groups, [&declared](GroupFilter const& group) {
            return !group.group.empty() &&
                   std::ranges::find(declared, group.group, &ColumnGroupInfo::id) == declared.end();
        });
        for (auto& group : _filterSpec.groups) {
            std::erase_if(group.columns,
                          [&](std::string const& columnId) { return !detail::findColumn(_columns, columnId); });
        }
        std::erase_if(_filterSpec.groups,
                      [](GroupFilter const& group) { return group.columns.empty() && group.group.empty(); });
        auto compiled = compile(_filterSpec);
        if (compiled) {
            _filter = std::make_shared<CompiledFilter const>(std::move(*compiled));
        } else {
            // A remaining entry no longer compiles for its column's new kind.
            _filterSpec = {};
            _filter = nullptr;
        }
    }

    // The shared guarded compile: an exception from the collator or date parser is an error.
    [[nodiscard]] std::expected<CompiledFilter, TableError> compile(FilterSpec const& spec) const {
        return compileFilterGuarded(spec, _columns, _options.services, _source->groups());
    }

    // ── scheduling ──────────────────────────────────────────────────────────

    // Whether the shown view is out of date with the requested state.
    [[nodiscard]] bool hasWork() const {
        return !_dirty.empty() || _epoch != _viewEpoch || _sort != _appliedSort || _filter != _appliedFilter ||
               _columnsVersion != _viewColumnsVersion || _retainedVersion != _appliedRetainedVersion;
    }

    // A repair merges the dirty rows into the shown order: only the rows'
    // contents changed since that order was computed. (A structural change
    // advances the epoch; every row is owed a `Changed` exactly while the
    // epoch is ahead of the view's.) A view with retained rows is computed whole: a repair merges into an order the
    // filter made, which retained rows are not part of.
    [[nodiscard]] bool canRepair() const {
        return !_dirty.empty() && _epoch == _viewEpoch && _sort == _appliedSort && _filter == _appliedFilter &&
               _columnsVersion == _viewColumnsVersion && _retained.empty() &&
               _retainedVersion == _appliedRetainedVersion;
    }

    // Whether a change since @p input was taken makes its result useless.
    [[nodiscard]] bool supersedes(detail::JobInput const& input, Ticket const& ticket) const {
        return _sort != input.sort || _filter != input.filter || _epoch != ticket.epoch ||
               _columnsVersion != ticket.columnsVersion || _retainedVersion != ticket.retainedVersion;
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
            input.previousUndecided = _undecided;
        }
        input.allChanged = _obligeAll;
        _obligeAll = false;
        input.resetThreshold = _options.resetThreshold;
        input.retained = _retained;
        ticket = Ticket{.epoch = _epoch,
                        .changeSeq = _absorbedSeq,
                        .columnsVersion = _columnsVersion,
                        .retainedVersion = _retainedVersion};
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
        if (_retainedVersion != _appliedRetainedVersion) {
            _retained = _appliedRetained;
            _retainedVersion = _appliedRetainedVersion;
        }
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
            return std::unexpected(*job.failure());
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
            afterJob(job->failure());
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
        _stats.filterRuns += result.filtered ? 1 : 0;
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
        _appliedRetained = input.retained;
        _appliedRetainedVersion = ticket.retainedVersion;
        _unmatched = std::move(result.unmatched);
        _trueView = std::move(result.view);
        auto const undecidedBefore = _undecided.size();
        _undecided = std::move(result.undecided);
        _viewSnapshot = input.snapshot;
        _relayoutPending = false;
        _viewIndexValid = false;
        _keyIndexValid = false;
        Applied out{
            .change = std::move(result.change),
            .armSettle = false,
            .undecided = _undecided.size() != undecidedBefore ? std::optional{_undecided.size()} : std::nullopt};
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
        deliver(Applied{.change = change, .armSettle = false, .undecided = std::nullopt});
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
            if (alive.expired()) {
                return false;
            }
        }
        if (applied.undecided && _onUndecidedChange) {
            auto const handler = _onUndecidedChange;
            handler(*applied.undecided);
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
    std::vector<RowId> _retained;
    std::uint64_t _retainedVersion = 0;
    // applied state: what the shown view was computed from
    std::vector<ColumnInfo> _viewColumns;
    std::shared_ptr<RowSnapshot const> _viewSnapshot;
    SortChain _appliedSort;
    FilterSpec _appliedFilterSpec;
    std::shared_ptr<CompiledFilter const> _appliedFilter;
    std::uint64_t _viewColumnsVersion = 0;
    std::uint64_t _viewEpoch = 0;
    std::uint64_t _viewChangeSeq = 0;       // notifications the view took in
    std::vector<std::uint32_t> _view;       // displayed
    std::vector<std::uint32_t> _trueView;   // sorted and filtered, without holds
    std::vector<std::uint32_t> _undecided;  // neither admitted nor rejected, source rows ascending
    std::vector<RowId> _appliedRetained;
    std::uint64_t _appliedRetainedVersion = 0;
    std::vector<std::uint32_t> _unmatched;  // retained rows of the view the filter rejects, source rows ascending
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
    std::function<void(std::size_t)> _onUndecidedChange;
    std::function<void(bool)> _onPending;
    std::function<void(TableError const&)> _onError;
    EngineStats _stats;
    async::CallbackScope _alive;
};

// NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)

}  // namespace morph::table

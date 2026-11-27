// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <algorithm>
#include <core/async/StopToken.hpp>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <morph/core/completion.hpp>
#include <morph/core/executor.hpp>
#include <morph/table/view_change.hpp>
#include <morph/ui/table_rows.hpp>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace morph::testing {

/// Rows for a mount test: every row present, row `i` keyed `i` with the record `{id: i, c0: "r<i>c0", …}`, built when
/// first read. `holdFrom` makes the rows at and after an index not fetched until `arrive`; `unload` and `fail` give a
/// row a level that has not loaded, or that failed to.
class FakeRows final : public ui::TableRows {
public:
    struct Visible {
        std::int64_t first = 0;
        std::int64_t count = 0;
        std::vector<std::string> columns;
    };

    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): `FakeRows(rows, columns, owner, kind)` is a test helper whose arguments read differently at each call; a swap would fail the case's own assertion
    FakeRows(std::int64_t rows, std::size_t columns, exec::IExecutor& owner,
             table::TotalKind kind = table::TotalKind::Exact)
        : _rows{rows}, _columns{columns}, _kind{kind}, _owner{&owner} {}

    [[nodiscard]] ui::TableTotal total() const override {
        return ui::TableTotal{.rows = _rows, .kind = _kind, .known = true};
    }

    [[nodiscard]] std::optional<ui::TableRowRef> row(std::int64_t index) const override {
        if (index < 0 || index >= _rows || (_hold && index >= *_hold)) {
            return std::nullopt;
        }
        std::int64_t const key = _keyAt ? _keyAt(index) : index;
        auto [found, made] = _records.try_emplace(index);
        if (made) {
            ui::ValueRecord record;
            record.emplace("id", ui::Value{key});
            for (std::size_t column = 0; column < _columns; ++column) {
                record.emplace("c" + std::to_string(column),
                               ui::Value{"r" + std::to_string(key) + "c" + std::to_string(column)});
            }
            found->second = Entry{.key = table::RowId{key}, .row = ui::Value{std::move(record)}};
        }
        return ui::TableRowRef{.key = found->second.key, .row = found->second.row};
    }

    [[nodiscard]] table::UnloadedLevels unloadedLevels(std::int64_t index) const override {
        auto const found = _levels.find(index);
        if (found == _levels.end()) {
            return {};
        }
        return table::UnloadedLevels{.levels = found->second.levels, .failed = found->second.failed};
    }

    // Records the call, and forgets the records far from it.
    void visible(std::int64_t first, std::int64_t count, std::span<std::string const> columns) override {
        ++visibleCalls;
        lastVisible = Visible{.first = first, .count = count, .columns = {columns.begin(), columns.end()}};
        std::erase_if(_records, [first, count](auto const& entry) {
            return entry.first < first - 200 || entry.first >= first + count + 200;
        });
    }

    async::Completion<std::expected<std::vector<table::RowId>, table::TableError>> keysOf(
        std::int64_t /*first*/, std::int64_t /*count*/) override {
        using Result = std::expected<std::vector<table::RowId>, table::TableError>;
        auto [completion, promise] = async::Completion<Result>::makeSettleable(_owner);
        promise.resolve(Result{std::vector<table::RowId>{}});
        return std::move(completion);
    }

    // Scans the view for the key, and answers on the owner.
    void locateRow(table::RowId key, ::core::async::StopToken stop,
                   std::function<void(ui::RowLocation const&)> done) override {
        std::optional<std::int64_t> found;
        for (std::int64_t index = 0; index < _rows && !found; ++index) {
            if (table::RowId{_keyAt ? _keyAt(index) : index} == key) {
                found = index;
            }
        }
        ++locateCalls;
        _owner->post([stop = std::move(stop), done = std::move(done), found] {
            if (!stop.stop_requested()) {
                done(ui::RowLocation{found});
            }
        });
    }

    // Nothing is fetched: every row the view has is held.
    async::Completion<std::expected<std::vector<ui::FetchedRow>, table::TableError>> fetchRows(
        std::vector<table::RowId> /*keys*/, std::vector<std::string> /*columns*/) override {
        using Result = std::expected<std::vector<ui::FetchedRow>, table::TableError>;
        auto [completion, promise] = async::Completion<Result>::makeSettleable(_owner);
        promise.resolve(Result{std::vector<ui::FetchedRow>{}});
        return std::move(completion);
    }

    std::expected<void, table::TableError> setSort(ui::SortChain /*sort*/) override { return {}; }
    std::expected<void, table::TableError> setFilter(table::FilterSpec /*filter*/) override { return {}; }
    async::Completion<std::expected<table::DistinctValues, table::TableError>> distinctValues(
        table::DistinctRequest /*request*/) override {
        using Result = std::expected<table::DistinctValues, table::TableError>;
        auto [completion, promise] = async::Completion<Result>::makeSettleable(_owner);
        promise.resolve(Result{table::DistinctValues{}});
        return std::move(completion);
    }
    [[nodiscard]] ui::TableCountsView counts() const override { return ui::TableCountsView{.view = total()}; }
    void onChange(std::function<void(table::ViewChange const&)> handler) override { _handler = std::move(handler); }
    [[nodiscard]] ui::RowStates& states() override { return _states; }
    [[nodiscard]] ui::RowStates const& states() const override { return _states; }
    void moveKey(table::RowId const& from, table::RowId const& to) override { _states.move(from, to); }

    // Rows at and after @p index are not fetched until `arrive`.
    void holdFrom(std::int64_t index) { _hold = index; }

    // The held rows arrive: a reset.
    void arrive() {
        _hold.reset();
        reset();
    }

    // A new order, as a sort gives: the row at view index `i` is keyed `keyAt(i)` from now; a reset.
    void reorder(std::function<std::int64_t(std::int64_t)> keyAt) {
        _keyAt = std::move(keyAt);
        reset();
    }

    // Announces a change of several operations, as an engine's view change is; the rows themselves stay as they are.
    void announce(std::vector<table::ViewOp> ops) {
        if (_handler) {
            _handler(table::ViewChange{.ops = std::move(ops)});
        }
    }

    // A new total: a reset.
    void setTotal(std::int64_t rows, table::TotalKind kind) {
        _rows = rows;
        _kind = kind;
        reset();
    }

    // Row @p index has not loaded @p level: a change of the row.
    void unload(std::int64_t index, std::string const& level) {
        add(_levels[index].levels, level);
        changed(index);
    }

    // Row @p index failed to load @p level: a change of the row.
    void fail(std::int64_t index, std::string const& level) {
        add(_levels[index].levels, level);
        add(_levels[index].failed, level);
        changed(index);
    }

    // NOLINTBEGIN(cppcoreguidelines-non-private-member-variables-in-classes): the fake rows' call counters and last window, which the cases read directly
    std::size_t visibleCalls = 0;
    std::size_t locateCalls = 0;
    Visible lastVisible;
    // NOLINTEND(cppcoreguidelines-non-private-member-variables-in-classes)

private:
    struct Entry {
        table::RowId key;
        ui::Value row;
    };
    struct Levels {
        std::vector<std::string> levels;
        std::vector<std::string> failed;
    };

    static void add(std::vector<std::string>& sorted, std::string const& level) {
        if (!std::ranges::binary_search(sorted, level)) {
            sorted.insert(std::ranges::upper_bound(sorted, level), level);
        }
    }

    void changed(std::int64_t index) {
        if (_handler) {
            _handler(table::ViewChange{.ops = {table::ViewOp{.kind = table::ViewOp::Kind::Changed,
                                                             .first = static_cast<std::size_t>(index),
                                                             .count = 1,
                                                             .to = 0}}});
        }
    }

    void reset() {
        _records.clear();
        if (_handler) {
            _handler(table::ViewChange{
                .ops = {table::ViewOp{.kind = table::ViewOp::Kind::Reset, .first = 0, .count = 0, .to = 0}}});
        }
    }

    std::int64_t _rows;
    std::size_t _columns;
    table::TotalKind _kind;
    exec::IExecutor* _owner;
    std::optional<std::int64_t> _hold;
    mutable std::map<std::int64_t, Entry> _records;
    std::map<std::int64_t, Levels> _levels;
    std::function<void(table::ViewChange const&)> _handler;
    ui::RowStates _states;
    std::function<std::int64_t(std::int64_t)> _keyAt;
};

}  // namespace morph::testing

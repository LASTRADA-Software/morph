// SPDX-License-Identifier: Apache-2.0

// The non-template definitions of <morph/table/level_cache.hpp>.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <concepts>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <morph/attributes.hpp>
#include <morph/core/change_types.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/logger.hpp>
#include <morph/table/data_source.hpp>
#include <morph/table/level_cache.hpp>
#include <morph/table/query.hpp>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

// NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access) -- column and level indices come from the plans built against the same lists

namespace morph::table::detail {

std::size_t cellsBytes(std::vector<Cell> const& cells) noexcept {
    std::size_t bytes = sizeof(std::vector<Cell>);
    for (auto const& cell : cells) {
        bytes += cellBytes(cell);
    }
    return bytes;
}

}  // namespace morph::table::detail

namespace morph::table {

LeveledSource::LeveledSource(std::shared_ptr<DataSource const> const& base, std::vector<ColumnInfo> columns,
                             std::vector<LevelInput> levels)
    : _columns{std::move(columns)}, _groups(base->groups().begin(), base->groups().end()) {
    auto const baseColumns = base->columns();
    std::vector<Plan> plans;
    plans.reserve(_columns.size());
    for (auto const& column : _columns) {
        Plan plan;
        if (column.level.empty()) {
            plan.base = detail::findColumn(baseColumns, column.id);
        } else {
            for (std::size_t input = 0; input < levels.size(); ++input) {
                // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): input < levels.size()
                if (levels[input].source == nullptr || levels[input].source->level() != column.level) {
                    continue;
                }
                // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): input < levels.size()
                if (auto const place = detail::findColumn(levels[input].source->columns(), column.id)) {
                    plan.level = input;
                    plan.place = *place;
                }
                break;
            }
        }
        plans.push_back(plan);
    }
    std::vector<Level> read;
    read.reserve(levels.size());
    for (auto& input : levels) {
        auto const width = input.source != nullptr ? input.source->columns().size() : 0;
        read.push_back(Level{.name = input.source != nullptr ? std::string{input.source->level()} : std::string{},
                             .snapshot = std::move(input.snapshot),
                             .width = width});
    }
    _snapshot = std::make_shared<Snapshot const>(base->snapshot(), std::move(plans), std::move(read));
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): row, then column, as DataSource::cell declares them.
Cell LeveledSource::Snapshot::cell(std::size_t row, std::size_t column) const {
    auto const& plan = _plans[column];
    if (plan.base) {
        return _base->cell(row, *plan.base);
    }
    if (!plan.level) {
        return Unloaded{};
    }
    auto const& level = _levels[*plan.level];
    std::vector<Cell> buffer(level.width);
    if (level.snapshot->read(_base->rowId(row), buffer) != LevelRead::Loaded) {
        return Unloaded{};
    }
    return std::move(buffer[plan.place]);
}

void LeveledSource::Snapshot::readColumn(std::size_t column, ColumnSink& sink, std::size_t first,
                                         std::size_t last) const {
    auto const& plan = _plans[column];
    if (plan.base) {
        _base->readColumn(*plan.base, sink, first, last);
        return;
    }
    constexpr std::size_t kRun = 256;
    std::vector<Cell> run;
    run.reserve(kRun);
    std::vector<Cell> buffer(plan.level ? _levels[*plan.level].width : 0);
    for (std::size_t begin = first; begin < last; begin += kRun) {
        auto const end = std::min(last, begin + kRun);
        run.clear();
        for (std::size_t row = begin; row < end; ++row) {
            if (plan.level && _levels[*plan.level].snapshot->read(_base->rowId(row), buffer) == LevelRead::Loaded) {
                run.push_back(std::move(buffer[plan.place]));
            } else {
                run.emplace_back(Unloaded{});
            }
        }
        sink.cells(begin, run);
    }
}

MORPH_PROCESS_UNIQUE LevelRead LeveledSource::Snapshot::state(std::size_t row, std::string_view level) const {
    for (auto const& entry : _levels) {
        if (entry.name == level) {
            thread_local std::vector<Cell> buffer;
            buffer.resize(entry.width);
            return entry.snapshot->read(_base->rowId(row), buffer);
        }
    }
    return LevelRead::Unloaded;
}

}  // namespace morph::table

// NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)

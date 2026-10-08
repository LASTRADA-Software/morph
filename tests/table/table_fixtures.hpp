// SPDX-License-Identifier: Apache-2.0
//
// Shared helpers for the morph::table tests: building sources tersely, and a
// collator that folds accents so accent-insensitive rules can be pinned
// without a toolkit.

#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <morph/core/executor.hpp>
#include <morph/table/engine.hpp>
#include <random>
#include <memory>
#include <morph/table/data_source.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tabletest {

using morph::table::Cell;
using morph::table::ColumnInfo;
using morph::table::ColumnKind;
using morph::table::RowId;

// A Rational from decimal text; zero on bad input, which the tests never pass.
inline morph::math::Rational dec(std::string_view text) {
    return morph::table::parseDecimal(text).value_or(morph::math::Rational{});
}

inline ColumnInfo col(std::string id, ColumnKind kind, std::string comparator = {}) {
    return ColumnInfo{.id = std::move(id), .kind = kind, .comparator = std::move(comparator), .format = {}};
}

// A source whose row keys are 0..n-1 (as int64) and whose cells are given row by row.
inline std::shared_ptr<morph::table::VectorSource> makeSource(std::vector<ColumnInfo> columns,
                                                              std::vector<std::vector<Cell>> rows) {
    auto source = std::make_shared<morph::table::VectorSource>(std::move(columns));
    std::vector<RowId> ids;
    ids.reserve(rows.size());
    for (std::size_t i = 0; i < rows.size(); ++i) {
        ids.emplace_back(static_cast<std::int64_t>(i));
    }
    source->setRows(std::move(ids), std::move(rows));
    return source;
}

// A one-column source.
inline std::shared_ptr<morph::table::VectorSource> column(ColumnKind kind, std::vector<Cell> cells,
                                                          std::string id = "c") {
    std::vector<std::vector<Cell>> rows;
    rows.reserve(cells.size());
    for (auto& cell : cells) {
        rows.push_back({std::move(cell)});
    }
    return makeSource({col(std::move(id), kind)}, std::move(rows));
}

// Folds the accented Latin-1 vowels the fixtures use to their base letter,
// then lowers ASCII: a stand-in for a locale collator at primary strength.
class AccentFoldingCollator final : public morph::table::TextCollator {
public:
    [[nodiscard]] std::string sortKey(std::string_view text) const override { return fold(text); }
    [[nodiscard]] std::string fold(std::string_view text) const override {
        static constexpr std::pair<std::string_view, char> kMap[] = {
            {"\xC3\xA9", 'e'}, {"\xC3\xA8", 'e'}, {"\xC3\x89", 'e'}, {"\xC3\xA1", 'a'}, {"\xC3\xA0", 'a'},
            {"\xC3\x81", 'a'}, {"\xC3\xB6", 'o'}, {"\xC3\x96", 'o'}, {"\xC3\xBC", 'u'}, {"\xC3\x9C", 'u'},
        };
        std::string out;
        for (std::size_t i = 0; i < text.size();) {
            bool mapped = false;
            for (auto const& [from, to] : kMap) {
                if (text.substr(i, from.size()) == from) {
                    out.push_back(to);
                    i += from.size();
                    mapped = true;
                    break;
                }
            }
            if (mapped) {
                continue;
            }
            char ch = text[i++];
            if (ch >= 'A' && ch <= 'Z') {
                ch = static_cast<char>(ch - 'A' + 'a');
            }
            out.push_back(ch);
        }
        return out;
    }
};

// The columns of the synthetic table used by the engine tests and benchmarks:
// a key, an integer, a decimal, a quantity in mixed units, accented text and a
// date, each with a share of empty or unparsable cells.
inline std::vector<ColumnInfo> syntheticColumns() {
    return {col("id", ColumnKind::Key),        col("n", ColumnKind::Integer), col("price", ColumnKind::Decimal),
            col("mass", ColumnKind::Quantity), col("name", ColumnKind::Text), col("on", ColumnKind::Date)};
}

// One synthetic row for key `id`; `rng` decides every cell.
inline std::vector<Cell> syntheticRow(std::int64_t id, std::mt19937& rng) {
    static constexpr std::string_view kWords[] = {"alpha",  "Beta",   "\xC3\xA9tage", "Gamma",  "delta",
                                                  "\xC3\x89lan", "omega", "Zeta",       "\xC3\xBC" "ber", "pH value"};
    auto const pick = [&](std::uint32_t n) { return static_cast<std::uint32_t>(rng() % n); };
    std::vector<Cell> row;
    row.emplace_back(id);
    // ~5% empty, ~3% unparsable, otherwise a value.
    auto const invalidity = [&]() -> int {
        auto const roll = pick(100);
        return roll < 5 ? 1 : (roll < 8 ? 2 : 0);
    };
    switch (invalidity()) {
        case 1: row.emplace_back(); break;
        case 2: row.emplace_back(std::string{"n/a"}); break;
        default: row.emplace_back(static_cast<std::int64_t>(pick(1000)) - 500); break;
    }
    switch (invalidity()) {
        case 1: row.emplace_back(); break;
        case 2: row.emplace_back(std::string{"1,5"}); break;
        default:
            row.emplace_back(morph::math::Rational{morph::math::Numerator{static_cast<std::int64_t>(pick(100000))},
                                                   morph::math::Denominator{100}, morph::math::DecimalPlaces{2}});
            break;
    }
    switch (invalidity()) {
        case 1: row.emplace_back(); break;
        case 2: row.emplace_back(std::string{"heavy"}); break;
        default: {
            // grams, kilograms or milligrams
            static constexpr std::int64_t kFactors[] = {1, 1000, 1};
            auto const unit = pick(3);
            auto const amount = morph::math::Rational{static_cast<std::int64_t>(pick(5000)), {}};
            auto const factor = unit == 2 ? morph::math::Rational{morph::math::Numerator{1}, morph::math::Denominator{1000}, {}}
                                          : morph::math::Rational{kFactors[unit], {}};
            row.emplace_back(morph::table::QuantityCell{.amount = amount, .toCanonical = factor});
            break;
        }
    }
    switch (invalidity()) {
        case 1: row.emplace_back(); break;
        default:
            row.emplace_back(std::string{kWords[pick(10)]} + " " + std::to_string(pick(500)));
            break;
    }
    switch (invalidity()) {
        case 1: row.emplace_back(); break;
        case 2: row.emplace_back(std::string{"someday"}); break;
        default: row.emplace_back(static_cast<std::int64_t>(19000 + pick(2000))); break;
    }
    return row;
}

inline std::shared_ptr<morph::table::VectorSource> syntheticSource(std::size_t rows, std::uint32_t seed) {
    std::mt19937 rng{seed};
    std::vector<std::vector<Cell>> cells;
    cells.reserve(rows);
    for (std::size_t i = 0; i < rows; ++i) {
        cells.push_back(syntheticRow(static_cast<std::int64_t>(i), rng));
    }
    return makeSource(syntheticColumns(), std::move(cells));
}

// An executor that only queues; the test runs what it queued, when it wants.
class ManualExecutor final : public morph::exec::IExecutor {
public:
    void post(std::function<void()> task) override {
        std::scoped_lock const lock{_mutex};
        _tasks.push_back(std::move(task));
    }
    [[nodiscard]] bool isSerial() const noexcept override { return false; }
    // Runs every task queued so far (not the ones they queue); returns how many ran.
    std::size_t runAll() {
        std::vector<std::function<void()>> tasks;
        {
            std::scoped_lock const lock{_mutex};
            tasks.swap(_tasks);
        }
        for (auto& task : tasks) {
            task();
        }
        return tasks.size();
    }
    [[nodiscard]] std::size_t queued() const {
        std::scoped_lock const lock{_mutex};
        return _tasks.size();
    }

private:
    mutable std::mutex _mutex;
    std::vector<std::function<void()>> _tasks;
};

// The keys of an engine's view, in view order.
inline std::vector<RowId> viewKeys(morph::table::Engine const& engine) {
    std::vector<RowId> out;
    for (std::size_t i = 0; i < engine.viewRowCount(); ++i) {
        out.push_back(engine.rowIdAt(i));
    }
    return out;
}

// A list model that follows an engine by replaying every ViewChange, as a
// renderer does; `check()` says whether it still equals the engine's view.
class FollowingModel {
public:
    explicit FollowingModel(morph::table::Engine& engine) : _engine{&engine}, _rows{viewKeys(engine)} {
        engine.onViewChange([this](morph::table::ViewChange const& change) {
            ++changes;
            if (change.isReset()) {
                ++resets;
            }
            last = change;
            auto const after = viewKeys(*_engine);
            morph::table::applyViewChange(_rows, change, std::span<RowId const>{after});
        });
    }
    [[nodiscard]] bool check() const { return _rows == viewKeys(*_engine); }
    std::size_t changes = 0;
    std::size_t resets = 0;
    morph::table::ViewChange last;

private:
    morph::table::Engine* _engine;
    std::vector<RowId> _rows;
};

// Pumps `owner` until `done()` or a generous timeout; returns done().
inline bool pumpUntil(morph::exec::MainThreadExecutor& owner, std::function<bool()> const& done) {
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds{60};
    while (!done() && std::chrono::steady_clock::now() < deadline) {
        owner.runFor(std::chrono::milliseconds{5});
    }
    return done();
}

}  // namespace tabletest

// SPDX-License-Identifier: Apache-2.0
//
// Shared helpers for the morph::table tests: building sources tersely, and a
// collator that folds accents so accent-insensitive rules can be pinned
// without a toolkit.

#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <morph/core/executor.hpp>
#include <morph/table/data_source.hpp>
#include <morph/table/engine.hpp>
#include <morph/table/query.hpp>
#include <mutex>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
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

// The same cell, places included: the same alternative and an equal value, and for a Rational (a quantity's
// amount and factor too) the same decimal places. `Cell == Cell` compares values only, since Rational's equality
// ignores places.
[[nodiscard]] inline bool sameCell(Cell const& a, Cell const& b) {
    if (a.index() != b.index()) {
        return false;
    }
    auto const sameRational = [](morph::math::Rational const& x, morph::math::Rational const& y) {
        return x == y && x.decimalPlaces == y.decimalPlaces;
    };
    if (auto const* const value = std::get_if<morph::math::Rational>(&a)) {
        return sameRational(*value, std::get<morph::math::Rational>(b));
    }
    if (auto const* const value = std::get_if<morph::table::QuantityCell>(&a)) {
        auto const& other = std::get<morph::table::QuantityCell>(b);
        return sameRational(value->amount, other.amount) && sameRational(value->toCanonical, other.toCanonical);
    }
    return a == b;
}

inline ColumnInfo col(std::string id, ColumnKind kind, std::string comparator = {}) {
    return ColumnInfo{
        .id = std::move(id), .kind = kind, .comparator = std::move(comparator), .format = {}, .level = {}};
}

// A column whose cells need data level `level`: the only kind of column whose cells may be `Unloaded`.
inline ColumnInfo levelCol(std::string id, ColumnKind kind, std::string level) {
    return ColumnInfo{.id = std::move(id), .kind = kind, .comparator = {}, .format = {}, .level = std::move(level)};
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

// A one-column source whose column `c` is at level "slow", so its cells may be `Unloaded`; keys 0..n-1.
inline std::shared_ptr<morph::table::VectorSource> slowColumn(ColumnKind kind, std::vector<Cell> cells) {
    std::vector<std::vector<Cell>> rows;
    rows.reserve(cells.size());
    for (auto& cell : cells) {
        rows.push_back({std::move(cell)});
    }
    return makeSource({levelCol("c", kind, "slow")}, std::move(rows));
}

// The distinct-values cases' source: status (Text), city (Text), qty (Integer); keys 0..5. A column named in
// `slow` is at level "slow"; each (row, column) of `unloaded` holds Cell{Unloaded{}} (its column must be slow).
//   0: "open",   "Berlin", 1        3: empty,  "Berlin", 4
//   1: "Open",   "Paris",  2        4: "open", "Paris",  "x" (an invalid integer)
//   2: "closed", "Berlin", 3        5: "hold", "Rome",   5
inline std::shared_ptr<morph::table::VectorSource> distinctSource(
    std::vector<std::string> const& slow = {}, std::vector<std::pair<std::size_t, std::size_t>> const& unloaded = {}) {
    using S = std::string;
    std::vector<std::vector<Cell>> rows{{S{"open"}, S{"Berlin"}, std::int64_t{1}},
                                        {S{"Open"}, S{"Paris"}, std::int64_t{2}},
                                        {S{"closed"}, S{"Berlin"}, std::int64_t{3}},
                                        {Cell{}, S{"Berlin"}, std::int64_t{4}},
                                        {S{"open"}, S{"Paris"}, S{"x"}},
                                        {S{"hold"}, S{"Rome"}, std::int64_t{5}}};
    for (auto const& [row, column] : unloaded) {
        rows.at(row).at(column) = Cell{morph::table::Unloaded{}};
    }
    auto const levelOf = [&slow](std::string const& id) {
        return std::ranges::find(slow, id) != slow.end() ? std::string{"slow"} : std::string{};
    };
    return makeSource(
        {levelCol("status", ColumnKind::Text, levelOf("status")), levelCol("city", ColumnKind::Text, levelOf("city")),
         levelCol("qty", ColumnKind::Integer, levelOf("qty"))},
        std::move(rows));
}

// A typed row whose companyA, companyB and companyC form the declared group "company". Reflected by glaze.
struct GroupRow {
    std::string companyA;
    std::string companyB;
    std::string companyC;
    std::string name;
    static constexpr std::array<std::string_view, 3> kCompany{"companyA", "companyB", "companyC"};
    static constexpr std::array<morph::table::ColumnGroup, 1> columnGroups() noexcept {
        return {{{.id = "company", .columns = kCompany}}};
    }
};

// A typed row whose `qty` is at level "stock": the row type of the typed level cases. Reflected by glaze.
struct LevelRow {
    std::int64_t id = 0;
    std::string name;
    std::int64_t qty = 0;
    static constexpr std::array<morph::table::ColumnLevel, 1> columnLevels() noexcept {
        return {{{.column = "qty", .level = "stock"}}};
    }
};

// Rows 0..count-1: id i, name "n<i>", qty 10 * i.
inline std::vector<LevelRow> levelRows(std::int64_t count) {
    std::vector<LevelRow> out;
    for (std::int64_t i = 0; i < count; ++i) {
        // NOLINTNEXTLINE(performance-inefficient-vector-operation): `out` is a fixture list built once; sizing it first would not change what the case checks
        out.push_back(LevelRow{.id = i, .name = "n" + std::to_string(i), .qty = 10 * i});
    }
    return out;
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
    static constexpr std::array<std::string_view, 10> kWords{
        "alpha", "Beta", "\303\251tage", "Gamma", "delta", "\303\211lan", "omega", "Zeta", "\303\274ber", "pH value"};
    auto const pick = [&](std::uint32_t bound) { return static_cast<std::uint32_t>(rng() % bound); };
    enum class Draw : std::uint8_t { Value, Empty, Unparsable };
    // ~5% empty, ~3% unparsable, otherwise a value.
    auto const draw = [&] {
        auto const roll = pick(100);
        if (roll < 5) {
            return Draw::Empty;
        }
        return roll < 8 ? Draw::Unparsable : Draw::Value;
    };
    std::vector<Cell> row;
    row.reserve(6);
    auto const add = [&](Draw what, Cell unparsable, auto&& value) {
        if (what == Draw::Empty) {
            row.emplace_back();
        } else if (what == Draw::Unparsable) {
            row.push_back(std::move(unparsable));
        } else {
            row.push_back(value());
        }
    };
    row.emplace_back(id);
    add(draw(), std::string{"n/a"}, [&] { return Cell{static_cast<std::int64_t>(pick(1000)) - 500}; });
    add(draw(), std::string{"1,5"}, [&] {
        return Cell{morph::math::Rational{morph::math::Numerator{static_cast<std::int64_t>(pick(100000))},
                                          morph::math::Denominator{100}, morph::math::DecimalPlaces{2}}};
    });
    add(draw(), std::string{"heavy"}, [&] {
        // grams, kilograms or milligrams
        auto const unit = pick(3);
        auto const amount = morph::math::Rational{static_cast<std::int64_t>(pick(5000)), {}};
        auto factor = morph::math::Rational{1, {}};
        if (unit == 1) {
            factor = morph::math::Rational{1000, {}};
        } else if (unit == 2) {
            factor = morph::math::Rational{morph::math::Numerator{1}, morph::math::Denominator{1000}, {}};
        }
        return Cell{morph::table::QuantityCell{.amount = amount, .toCanonical = factor}};
    });
    // Text has no unparsable value: an unparsable draw is a value too.
    auto const textDraw = draw();
    add(textDraw == Draw::Empty ? Draw::Empty : Draw::Value, Cell{}, [&] {
        return Cell{std::string{kWords.at(pick(static_cast<std::uint32_t>(kWords.size())))} + " " +
                    std::to_string(pick(500))};
    });
    add(draw(), std::string{"someday"}, [&] { return Cell{static_cast<std::int64_t>(19000 + pick(2000))}; });
    return row;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters) -- row count, then seed
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
        for (auto const& task : tasks) {
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
    out.reserve(engine.viewRowCount());
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
            ++_changes;
            if (change.isReset()) {
                ++_resets;
            }
            _last = change;
            auto const after = viewKeys(*_engine);
            morph::table::applyViewChange(_rows, change, std::span<RowId const>{after});
        });
    }
    [[nodiscard]] bool check() const { return _rows == viewKeys(*_engine); }
    [[nodiscard]] std::size_t changes() const { return _changes; }
    [[nodiscard]] std::size_t resets() const { return _resets; }
    [[nodiscard]] morph::table::ViewChange const& last() const { return _last; }

private:
    morph::table::Engine* _engine;
    std::vector<RowId> _rows;
    std::size_t _changes = 0;
    std::size_t _resets = 0;
    morph::table::ViewChange _last;
};

// A list model that also keeps the text of one column per row, as a renderer
// caches what it painted: Inserted, Changed and Reset re-read it, Moved
// carries it along. `stale()` counts rows whose text differs from the engine's.
class ContentModel {
public:
    ContentModel(morph::table::Engine& engine, std::size_t column) : _engine{&engine}, _column{column} {
        for (std::size_t i = 0; i < engine.viewRowCount(); ++i) {
            _rows.emplace_back(engine.rowIdAt(i), text(i));
        }
        engine.onViewChange([this](morph::table::ViewChange const& change) { follow(change); });
    }
    // Rows painted with old text; -1 when the order itself diverged.
    [[nodiscard]] long stale() const {
        if (_rows.size() != _engine->viewRowCount()) {
            return -1;
        }
        long count = 0;
        for (std::size_t i = 0; i < _rows.size(); ++i) {
            if (_rows[i].first != _engine->rowIdAt(i)) {
                return -1;
            }
            count += _rows[i].second == text(i) ? 0 : 1;
        }
        return count;
    }
    [[nodiscard]] std::size_t changes() const { return _changes; }
    [[nodiscard]] morph::table::ViewChange const& last() const { return _last; }

private:
    [[nodiscard]] std::string text(std::size_t viewRow) const {
        return morph::table::displayText(_engine->cellAt(viewRow, _column));
    }
    void follow(morph::table::ViewChange const& change) {
        using Kind = morph::table::ViewOp::Kind;
        ++_changes;
        _last = change;
        for (auto const& op : change.ops) {
            auto const first = static_cast<std::ptrdiff_t>(op.first);
            switch (op.kind) {
                case Kind::Removed:
                    _rows.erase(_rows.begin() + first, _rows.begin() + first + static_cast<std::ptrdiff_t>(op.count));
                    break;
                case Kind::Moved: {
                    auto moved = std::move(_rows[op.first]);
                    _rows.erase(_rows.begin() + first);
                    _rows.insert(_rows.begin() + static_cast<std::ptrdiff_t>(op.to), std::move(moved));
                    break;
                }
                case Kind::Inserted:
                    for (std::size_t i = op.first; i < op.first + op.count; ++i) {
                        _rows.insert(_rows.begin() + static_cast<std::ptrdiff_t>(i), {_engine->rowIdAt(i), text(i)});
                    }
                    break;
                case Kind::Changed:
                    for (std::size_t i = op.first; i < op.first + op.count; ++i) {
                        _rows[i].second = text(i);
                    }
                    break;
                case Kind::Reset:
                default:
                    _rows.clear();
                    for (std::size_t i = 0; i < _engine->viewRowCount(); ++i) {
                        _rows.emplace_back(_engine->rowIdAt(i), text(i));
                    }
                    break;
            }
        }
    }

    morph::table::Engine* _engine;
    std::size_t _column;
    std::vector<std::pair<RowId, std::string>> _rows;
    std::size_t _changes = 0;
    morph::table::ViewChange _last;
};

// Runs `owner` and `worker` until neither has anything queued.
inline void drain(ManualExecutor& owner, ManualExecutor* worker = nullptr) {
    while (owner.runAll() + (worker != nullptr ? worker->runAll() : 0) > 0) {
    }
}

// Pumps `owner` until `done()` or a generous timeout; returns done().
inline bool pumpUntil(morph::exec::MainThreadExecutor& owner, std::function<bool()> const& done) {
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds{60};
    while (!done() && std::chrono::steady_clock::now() < deadline) {
        owner.runFor(std::chrono::milliseconds{5});
    }
    return done();
}

}  // namespace tabletest

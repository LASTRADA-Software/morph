// SPDX-License-Identifier: Apache-2.0
//
// morph::table measurements (spec 7 §15). Every case is tagged [.benchmark]:
// hidden, so ctest never runs it; run it by tag on a Release build:
//
//   morph_table_tests "[.benchmark]"
//
// Each scenario runs several times on a fresh engine and prints one Markdown
// table row: median and the 10th-90th percentile spread, in milliseconds.
// Catch2's BENCHMARK reports a mean and a standard deviation; the spec asks
// for medians, so the cases time themselves.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <glaze/glaze.hpp>
#include <memory>
#include <morph/core/executor.hpp>
#include <morph/core/wire.hpp>
#include <morph/table/engine.hpp>
#include <morph/table/query.hpp>
#include <print>
#include <random>
#include <string>
#include <thread>
#include <type_traits>
#include <variant>
#include <vector>

#include "table/table_fixtures.hpp"
#include "table/wide_table_model.hpp"

using namespace morph::table;

namespace {

using Clock = std::chrono::steady_clock;

constexpr auto kAsc = SortDirection::Ascending;
constexpr auto kDesc = SortDirection::Descending;

struct Spread {
    double median = 0;
    double p10 = 0;
    double p90 = 0;
};

Spread spreadOf(std::vector<double> samples) {
    std::ranges::sort(samples);
    auto const at = [&](double q) {
        auto const index = static_cast<std::size_t>(std::lround(q * static_cast<double>(samples.size() - 1)));
        return samples[std::min(index, samples.size() - 1)];
    };
    return Spread{.median = at(0.5), .p10 = at(0.1), .p90 = at(0.9)};
}

int runsFor(std::size_t rows) {
    if (rows >= 100000) {
        return 7;
    }
    return rows >= 10000 ? 15 : 31;
}

void printHeader(char const* title) {
    std::println("\n{}\n\n| scenario | rows | median ms | p10-p90 ms |\n|---|---:|---:|---:|", title);
}

void printRow(std::string const& scenario, std::size_t rows, Spread const& spread) {
    std::println("| {} | {} | {:.2f} | {:.2f}-{:.2f} |", scenario, rows, spread.median, spread.p10, spread.p90);
    static_cast<void>(std::fflush(stdout));
}

// Times `body` over `runs` fresh engines on `source`; the engine computes in
// the calling thread (no owner), so the time is the work itself.
// NOLINTBEGIN(bugprone-easily-swappable-parameters) -- setup, then the body that is timed
void measure(std::string const& scenario, std::shared_ptr<VectorSource> const& source,
             std::function<void(Engine&)> const& setup, std::function<void(Engine&)> const& body) {
    // NOLINTEND(bugprone-easily-swappable-parameters)
    auto const rows = source->snapshot()->rowCount();
    std::vector<double> samples;
    for (int run = 0; run < runsFor(rows); ++run) {
        Engine engine{source};
        setup(engine);
        auto const began = Clock::now();
        body(engine);
        samples.push_back(std::chrono::duration<double, std::milli>(Clock::now() - began).count());
    }
    printRow(scenario, rows, spreadOf(samples));
}

FilterSpec filterOn(std::string column, FilterEntry entry) {
    FilterSpec spec;
    spec.columns.emplace(std::move(column),
                         ColumnFilter{.combine = Combine::Any, .include = {std::move(entry)}, .exclude = {}});
    return spec;
}

FilterSpec groupFilter() {
    FilterSpec spec;
    spec.groups.push_back(GroupFilter{
        .columns = {"name", "n", "price"}, .combine = Combine::Any, .include = {FilterEntry{.contains = "ta"}}});
    return spec;
}

void require(std::expected<void, TableError> const& result) { REQUIRE(result.has_value()); }

// Runs owner tasks as soon as they are posted, so a timing measures the work
// and not a polling interval.
bool spinUntil(morph::exec::MainThreadExecutor& owner, std::function<bool()> const& done) {
    auto const deadline = Clock::now() + std::chrono::seconds{60};
    while (!done() && Clock::now() < deadline) {
        if (!owner.runOnce()) {
            std::this_thread::yield();
        }
    }
    return done();
}

constexpr std::array<std::size_t, 3> kSizes{1000, 10000, 100000};

}  // namespace

TEST_CASE("table benchmark: sort and filter on the calling thread", "[.benchmark][table]") {
    printHeader("Sort and filter, computed in the calling thread (keys built inside the timing)");
    auto const none = [](Engine&) {};
    for (auto const size : kSizes) {
        auto const source = tabletest::syntheticSource(size, 42);
        measure("sort: one numeric key", source, none,
                [](Engine& e) { require(e.setSort({{.column = "price", .dir = kAsc}})); });
        measure("sort: one text key", source, none,
                [](Engine& e) { require(e.setSort({{.column = "name", .dir = kAsc}})); });
        measure("sort: three keys", source, none, [](Engine& e) {
            require(e.setSort(
                {{.column = "on", .dir = kAsc}, {.column = "price", .dir = kDesc}, {.column = "mass", .dir = kAsc}}));
        });
        measure("filter: text contains", source, none,
                [](Engine& e) { require(e.setFilter(filterOn("name", FilterEntry{.contains = "ETA"}))); });
        measure("filter: numeric compare", source, none,
                [](Engine& e) { require(e.setFilter(filterOn("price", FilterEntry{.ge = "500"}))); });
        measure("filter: group", source, none, [](Engine& e) { require(e.setFilter(groupFilter())); });
        measure("sort then filter", source, none, [](Engine& e) {
            require(e.setSort({{.column = "price", .dir = kAsc}}));
            require(e.setFilter(filterOn("n", FilterEntry{.gt = "0"})));
        });
        measure("filter then sort", source, none, [](Engine& e) {
            require(e.setFilter(filterOn("n", FilterEntry{.gt = "0"})));
            require(e.setSort({{.column = "price", .dir = kAsc}}));
        });
        measure(
            "toggle one filter ten times, sorted", source,
            [](Engine& e) {
                require(e.setSort({{.column = "price", .dir = kAsc}}));
                require(e.setFilter(filterOn("n", FilterEntry{.gt = "0"})));
            },
            [](Engine& e) {
                for (int i = 0; i < 10; ++i) {
                    require(e.setFilter(i % 2 == 0 ? FilterSpec{} : filterOn("n", FilterEntry{.gt = "0"})));
                }
            });
    }
}

TEST_CASE("table benchmark: 1,000 cell updates while sorted", "[.benchmark][table]") {
    printHeader("1,000 row updates while sorted, coalesced into one repair on the owner");
    for (auto const size : kSizes) {
        std::vector<double> samples;
        std::size_t fullSorts = 0;
        for (int run = 0; run < runsFor(size); ++run) {
            auto const source = tabletest::syntheticSource(size, 42);
            morph::exec::MainThreadExecutor owner;
            Engine engine{source, EngineOptions{.owner = &owner, .worker = nullptr}};
            require(engine.setSort({{.column = "price", .dir = kAsc}}));
            REQUIRE(spinUntil(owner, [&] { return !engine.pending(); }));
            // NOLINTNEXTLINE(bugprone-random-generator-seed,cert-msc32-c,cert-msc51-cpp) -- a fixed seed keeps the case reproducible
            std::mt19937 rng{7};
            std::vector<std::pair<std::size_t, std::vector<Cell>>> updates;
            for (int i = 0; i < 1000; ++i) {
                auto const row = rng() % size;
                updates.emplace_back(row, tabletest::syntheticRow(static_cast<std::int64_t>(row), rng));
            }
            auto const sortsBefore = engine.stats().fullSorts;
            auto const began = Clock::now();
            for (auto& [row, cells] : updates) {
                source->updateRow(row, std::move(cells));
            }
            REQUIRE(spinUntil(owner, [&] { return !engine.pending() && engine.stats().repairs > 0; }));
            owner.drain();
            samples.push_back(std::chrono::duration<double, std::milli>(Clock::now() - began).count());
            fullSorts += engine.stats().fullSorts - sortsBefore;
        }
        CHECK(fullSorts == 0);
        printRow("1,000 updates while sorted", size, spreadOf(samples));
    }
}

TEST_CASE("table benchmark: owner blocking with and without a worker", "[.benchmark][table]") {
    std::println(
        "\nThe longest single owner step, per request (sort on three keys, then a text filter)\n\n| execution | rows "
        "| worst owner step ms (median of runs) | worst of all runs ms | request to result ms (median) "
        "|\n|---|---:|---:|---:|---:|");
    morph::exec::ThreadPoolExecutor pool{2};
    for (auto const size : kSizes) {
        auto const source = tabletest::syntheticSource(size, 42);
        for (bool const withWorker : {false, true}) {
            std::vector<double> worst;
            std::vector<double> latency;
            for (int run = 0; run < runsFor(size); ++run) {
                morph::exec::MainThreadExecutor owner;
                Engine engine{source, EngineOptions{.owner = &owner, .worker = withWorker ? &pool : nullptr}};
                auto const began = Clock::now();
                require(engine.setSort({{.column = "on", .dir = kAsc},
                                        {.column = "price", .dir = kDesc},
                                        {.column = "mass", .dir = kAsc}}));
                REQUIRE(spinUntil(owner, [&] { return !engine.pending(); }));
                require(engine.setFilter(filterOn("name", FilterEntry{.contains = "eta"})));
                REQUIRE(spinUntil(owner, [&] { return !engine.pending(); }));
                latency.push_back(std::chrono::duration<double, std::milli>(Clock::now() - began).count());
                worst.push_back(static_cast<double>(engine.stats().maxOwnerStepMicros) / 1000.0);
            }
            auto const spread = spreadOf(worst);
            std::println("| {} | {} | {:.2f} | {:.2f} | {:.2f} |", withWorker ? "worker" : "owner steps (8 ms budget)",
                         size, spread.median, *std::ranges::max_element(worst), spreadOf(latency).median);
            static_cast<void>(std::fflush(stdout));
        }
    }
}

// Ten small cells, the row spec 7 §9 sizes an envelope with. Reflected by
// glaze, so not in an anonymous namespace.
namespace tbench {

struct SmallRow {
    std::int64_t id = 0;
    std::string code;
    std::int64_t count = 0;
    std::int64_t day = 0;
    double reading = 0;
    bool approved = false;
    std::string status;
    morph::math::Rational price;
    std::int64_t owner = 0;
    std::string note;
};

}  // namespace tbench

using tbench::SmallRow;

namespace {

std::string envelopeFor(std::size_t rows) {
    Page<SmallRow> page;
    page.total = static_cast<std::int64_t>(rows);
    for (std::size_t i = 0; i < rows; ++i) {
        auto const n = static_cast<std::int64_t>(i);
        page.rows.push_back(
            SmallRow{.id = 100000 + n,
                     .code = "S-" + std::to_string(10000 + (i % 90000)),
                     .count = n % 1000,
                     .day = 20000 + (n % 365),
                     .reading = static_cast<double>(i % 100) / 7.0,
                     .approved = i % 2 == 0,
                     .status = i % 3 == 0 ? "open" : "closed",
                     .price = morph::math::Rational{morph::math::Numerator{n % 100000}, morph::math::Denominator{100},
                                                    morph::math::DecimalPlaces{2}},
                     .owner = n % 50,
                     .note = "note " + std::to_string(i % 1000)});
    }
    std::string body;
    REQUIRE_FALSE(glz::write_json(page, body));
    return morph::wire::encode(morph::wire::makeOk(1, std::move(body)));
}

}  // namespace

TEST_CASE("table benchmark: rows of ten small cells per envelope", "[.benchmark][table]") {
    auto const small = envelopeFor(1000).size();
    auto const large = envelopeFor(2000).size();
    auto const perRow = static_cast<double>(large - small) / 1000.0;
    auto const overhead = static_cast<double>(small) - (perRow * 1000.0);
    auto const estimate =
        static_cast<std::size_t>((static_cast<double>(morph::wire::kMaxEnvelopeBytes) - overhead) / perRow);
    // Later rows have longer numbers, so the linear estimate is close but not
    // exact: search around it for the largest count that fits, then check it
    // against the real decoder in both directions.
    std::size_t low = estimate - (estimate / 10);
    std::size_t high = estimate + (estimate / 10);
    REQUIRE(envelopeFor(low).size() <= morph::wire::kMaxEnvelopeBytes);
    REQUIRE(envelopeFor(high).size() > morph::wire::kMaxEnvelopeBytes);
    while (high - low > 1) {
        auto const middle = low + ((high - low) / 2);
        (envelopeFor(middle).size() <= morph::wire::kMaxEnvelopeBytes ? low : high) = middle;
    }
    auto const fit = low;
    CHECK(morph::wire::decode(envelopeFor(fit)).kind == "ok");
    CHECK_THROWS(morph::wire::decode(envelopeFor(fit + 1)));
    std::println(
        "\nRows per envelope (kMaxEnvelopeBytes = {})\n\n| bytes per row in the envelope | rows that fit "
        "|\n|---:|---:|\n| {:.1f} | {} |",
        morph::wire::kMaxEnvelopeBytes, perRow, fit);
    static_cast<void>(std::fflush(stdout));
}

// ── Step 0 baselines over the wide-table fixture ────────────────────────────
// docs/spec/table/table.md, "Performance": each case first REQUIREs that the path it times was reached, then prints
// its rows as the cases above do.

namespace {

constexpr std::int64_t kWideRows = 100'000;

TableQuery wideQuery(std::vector<SortKey> sort, FilterSpec filters, std::int64_t offset) {
    TableQuery query;
    query.sort = std::move(sort);
    query.filters = std::move(filters);
    query.page = {.offset = offset, .limit = 200};
    return query;
}

double millisSince(Clock::time_point began) {
    return std::chrono::duration<double, std::milli>(Clock::now() - began).count();
}

void printBytes(std::string const& what, std::size_t rows, std::size_t bytes) {
    std::println("| {} | {} | {} | {:.1f} |", what, rows, bytes, static_cast<double>(bytes) / (1024.0 * 1024.0));
    static_cast<void>(std::fflush(stdout));
}

}  // namespace

TEST_CASE("table baseline: S1 S2 S7 over the wide table", "[.benchmark][baseline]") {
    bench::WideTableSource const wide{{.rows = kWideRows, .columns = 300, .seed = 1}};
    auto const s1 = wideQuery({{.column = "d000", .dir = kAsc}}, {}, 0);
    auto const s2 = wideQuery({}, filterOn("t001", FilterEntry{.contains = "ka"}), 0);
    {
        auto const page = apply(wide, s1);
        REQUIRE(page.has_value());
        REQUIRE(page->rows.size() == 200);
        REQUIRE(page->total == kWideRows);
        auto const filtered = apply(wide, s2);
        REQUIRE(filtered.has_value());
        REQUIRE(filtered->total.has_value());
        REQUIRE(*filtered->total > 0);
        REQUIRE(*filtered->total < kWideRows);
    }
    printHeader("Step 0 baselines: apply over the wide table, 100,000 x 300, in the calling thread");
    auto const time = [&](std::string const& scenario, TableQuery const& query) {
        std::vector<double> samples;
        for (int run = 0; run < runsFor(kWideRows); ++run) {
            auto const began = Clock::now();
            auto const page = apply(wide, query);
            samples.push_back(millisSince(began));
            REQUIRE(page.has_value());
        }
        printRow(scenario, kWideRows, spreadOf(samples));
    };
    time("S1: sort d000 asc, page of 200", s1);
    time("S2: filter t001 contains \"ka\"", s2);

    // S7: ten consecutive pages of S1's query, each a separate apply.
    std::vector<double> totals;
    std::vector<double> perCall;
    for (int run = 0; run < runsFor(kWideRows); ++run) {
        std::vector<RowId> firstKeys;
        auto const snapshot = wide.snapshot();
        auto const began = Clock::now();
        for (std::int64_t page = 0; page < 10; ++page) {
            auto const call = Clock::now();
            auto const rows = apply(wide, wideQuery({{.column = "d000", .dir = kAsc}}, {}, page * 200));
            perCall.push_back(millisSince(call));
            REQUIRE(rows.has_value());
            REQUIRE_FALSE(rows->rows.empty());
            firstKeys.push_back(snapshot->rowId(rows->rows.front()));
        }
        totals.push_back(millisSince(began));
        std::ranges::sort(firstKeys);
        REQUIRE(std::ranges::adjacent_find(firstKeys) == firstKeys.end());
    }
    printRow("S7: ten pages of S1, total", kWideRows, spreadOf(totals));
    printRow("S7: ten pages of S1, per call", kWideRows, spreadOf(perCall));
}

#ifndef __EMSCRIPTEN__
// Native only: the VectorSource of the same rows holds near 1.7 GB of cells.
TEST_CASE("table baseline: S8 resident bytes", "[.benchmark][baseline]") {
    bench::WideTableSource const wide{{.rows = kWideRows, .columns = 300, .seed = 1}};
    auto const snapshot = wide.snapshot();
    auto const columns = wide.columns().size();
    std::vector<RowId> ids;
    std::vector<std::vector<Cell>> rows;
    ids.reserve(snapshot->rowCount());
    rows.reserve(snapshot->rowCount());
    std::size_t cellBytes = 0;
    for (std::size_t row = 0; row < snapshot->rowCount(); ++row) {
        ids.push_back(snapshot->rowId(row));
        std::vector<Cell> cells;
        cells.reserve(columns);
        for (std::size_t column = 0; column < columns; ++column) {
            cells.push_back(snapshot->cell(row, column));
            cellBytes += bench::approximateCellBytes(cells.back());
        }
        rows.push_back(std::move(cells));
    }
    VectorSource cellsSource{std::vector<ColumnInfo>(wide.columns().begin(), wide.columns().end())};
    cellsSource.setRows(std::move(ids), std::move(rows));
    REQUIRE(cellsSource.snapshot()->rowCount() == static_cast<std::size_t>(kWideRows));
    std::println(
        "\nS8: resident bytes of the wide table, 100,000 x 300\n\n| held as | rows | bytes | MiB |\n|---|---:|---:|---:|");
    printBytes("S8: the fixture's column store (residentBytes)", snapshot->rowCount(), wide.residentBytes());
    printBytes("S8: a VectorSource of the same rows (approximateCellBytes)", snapshot->rowCount(), cellBytes);
}
#endif

TEST_CASE("table baseline: B1 client mode with no worker", "[.benchmark][baseline]") {
    std::println(
        "\nB1: client mode, an owner and no worker, WideTableSource of 30 columns (sort a000 asc, d000 desc, i001 asc, "
        "then filter t001 contains \"ka\")\n\n| rows | request to result ms (median) | p10-p90 ms | worst owner step ms "
        "(median of runs) | p10-p90 ms |\n|---:|---:|---:|---:|---:|");
    for (std::int64_t const rows : {std::int64_t{10'000}, std::int64_t{20'000}, kWideRows}) {
        auto const source =
            std::make_shared<bench::WideTableSource>(bench::WideTableOptions{.rows = rows, .columns = 30, .seed = 1});
        std::vector<double> latency;
        std::vector<double> worst;
        for (int run = 0; run < runsFor(static_cast<std::size_t>(rows)); ++run) {
            morph::exec::MainThreadExecutor owner;
            Engine engine{source, EngineOptions{.owner = &owner, .worker = nullptr}};
            REQUIRE(spinUntil(owner, [&] { return !engine.pending(); }));
            auto const firstBefore = engine.rowIdAt(0);
            auto const began = Clock::now();
            require(engine.setSort(
                {{.column = "a000", .dir = kAsc}, {.column = "d000", .dir = kDesc}, {.column = "i001", .dir = kAsc}}));
            REQUIRE(spinUntil(owner, [&] { return !engine.pending(); }));
            REQUIRE(engine.rowIdAt(0) != firstBefore);
            require(engine.setFilter(filterOn("t001", FilterEntry{.contains = "ka"})));
            REQUIRE(spinUntil(owner, [&] { return !engine.pending(); }));
            latency.push_back(millisSince(began));
            worst.push_back(static_cast<double>(engine.stats().maxOwnerStepMicros) / 1000.0);
        }
        auto const l = spreadOf(latency);
        auto const w = spreadOf(worst);
        std::println("| {} | {:.2f} | {:.2f}-{:.2f} | {:.2f} | {:.2f}-{:.2f} |", rows, l.median, l.p10, l.p90, w.median,
                     w.p10, w.p90);
        static_cast<void>(std::fflush(stdout));
    }
}

namespace {

// A page of the wide table as row objects keyed by column id, each cell written as glaze writes a typed member of
// its kind: a decimal as the Rational object, a quantity as its amount in the column's unit, an empty cell as null.
std::string typedPage(bench::WideTableSource const& source, std::size_t rows, std::size_t& cells) {
    auto const snapshot = source.snapshot();
    auto const columns = source.columns();
    std::string out = "[";
    std::string buffer;
    for (std::size_t row = 0; row < rows; ++row) {
        out += row == 0 ? "{" : ",{";
        for (std::size_t column = 0; column < columns.size(); ++column) {
            if (column > 0) {
                out += ',';
            }
            out += '"';
            out += columns[column].id;
            out += "\":";
            Cell const cell = snapshot->cell(row, column);
            buffer.clear();
            bool const failed = std::visit(
                [&]<typename T>(T const& value) -> bool {
                    if constexpr (std::is_same_v<T, std::monostate>) {
                        buffer = "null";
                        return false;
                    } else if constexpr (std::is_same_v<T, QuantityCell>) {
                        return static_cast<bool>(glz::write_json(value.amount, buffer));
                    } else {
                        return static_cast<bool>(glz::write_json(value, buffer));
                    }
                },
                cell);
            REQUIRE_FALSE(failed);
            out += buffer;
            ++cells;
        }
        out += '}';
    }
    out += ']';
    return out;
}

}  // namespace

TEST_CASE("table baseline: W7 as a typed page", "[.benchmark][baseline]") {
    bench::WideTableSource const wide{{.rows = 200, .columns = 300, .seed = 1}};
    std::size_t cells = 0;
    auto const bytes = typedPage(wide, 200, cells).size();
    REQUIRE(cells == 60'000);
    std::vector<double> samples;
    for (int run = 0; run < 15; ++run) {
        std::size_t counted = 0;
        auto const began = Clock::now();
        auto const page = typedPage(wide, 200, counted);
        samples.push_back(millisSince(began));
        REQUIRE(page.size() == bytes);
    }
    auto const spread = spreadOf(samples);
    std::println(
        "\nW7: a page of 200 rows x 300 columns as typed row objects\n\n| bytes | encode ms (median) | p10-p90 ms "
        "|\n|---:|---:|---:|\n| {} | {:.2f} | {:.2f}-{:.2f} |",
        bytes, spread.median, spread.p10, spread.p90);
    static_cast<void>(std::fflush(stdout));
}

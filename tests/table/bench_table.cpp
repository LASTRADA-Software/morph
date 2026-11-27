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
#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <format>
#include <functional>
#include <future>
#include <glaze/glaze.hpp>
#include <latch>
#include <memory>
#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/remote.hpp>
#include <morph/core/wire.hpp>
#include <morph/detail/reflected_member.hpp>
#include <morph/session/session.hpp>
#include <morph/table/description.hpp>
#include <morph/table/engine.hpp>
#include <morph/table/level_cache.hpp>
#include <morph/table/query.hpp>
#include <morph/table/state_store.hpp>
#include <morph/table/view_cache.hpp>
#include <morph/ui/locale_table.hpp>
#include <morph/ui/measure.hpp>
#include <optional>
#include <print>
#include <random>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "table/table_fixtures.hpp"
#include "table/wide_table_model.hpp"
#include "test_support.hpp"

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

#ifndef __EMSCRIPTEN__
void printBytes(std::string const& what, std::size_t rows, std::size_t bytes) {
    std::println("| {} | {} | {} | {:.1f} |", what, rows, bytes, static_cast<double>(bytes) / (1024.0 * 1024.0));
    static_cast<void>(std::fflush(stdout));
}
#endif

}  // namespace

namespace {

// What every apply over the wide fixture is given: its unit registry, so a query on an `m` column is answered.
ApplyOptions wideOptions() {
    ApplyOptions out;
    out.services.units = bench::wideUnits();
    return out;
}

// The same for an engine over the fixture.
EngineOptions wideEngine(morph::exec::IExecutor* owner = nullptr) {
    EngineOptions out;
    out.owner = owner;
    out.services.units = bench::wideUnits();
    return out;
}

}  // namespace

TEST_CASE("table baseline: S1 S2 S7 over the wide table", "[.benchmark][baseline]") {
    bench::WideTableSource const wide{{.rows = kWideRows, .columns = 300, .seed = 1}};
    auto const s1 = wideQuery({{.column = "d000", .dir = kAsc}}, {}, 0);
    auto const s2 = wideQuery({}, filterOn("t001", FilterEntry{.contains = "ka"}), 0);
    {
        auto const page = apply(wide, s1, wideOptions());
        REQUIRE(page.has_value());
        REQUIRE(page->rows.size() == 200);
        REQUIRE(page->total == kWideRows);
        auto const filtered = apply(wide, s2, wideOptions());
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
            auto const page = apply(wide, query, wideOptions());
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
            auto const rows = apply(wide, wideQuery({{.column = "d000", .dir = kAsc}}, {}, page * 200), wideOptions());
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
// Native only: the VectorSource of the same rows holds about 2 GiB of cells.
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
        "\nS8: resident bytes of the wide table, 100,000 x 300\n\n| held as | rows | bytes | MiB "
        "|\n|---|---:|---:|---:|");
    printBytes("S8: the fixture's column store (residentBytes)", snapshot->rowCount(), wide.residentBytes());
    printBytes("S8: a VectorSource of the same rows (approximateCellBytes)", snapshot->rowCount(), cellBytes);
}
#endif

TEST_CASE("table baseline: B1 client mode with no worker", "[.benchmark][baseline]") {
    std::println(
        "\nB1: client mode, an owner and no worker, WideTableSource of 30 columns (sort a000 asc, d000 desc, i001 "
        "asc, "
        "then filter t001 contains \"ka\")\n\n| rows | request to result ms (median) | p10-p90 ms | worst owner step "
        "ms "
        "(median of runs) | p10-p90 ms |\n|---:|---:|---:|---:|---:|");
    for (std::int64_t const rows : {std::int64_t{10'000}, std::int64_t{20'000}, kWideRows}) {
        auto const source =
            std::make_shared<bench::WideTableSource>(bench::WideTableOptions{.rows = rows, .columns = 30, .seed = 1});
        std::vector<double> latency;
        std::vector<double> worst;
        for (int run = 0; run < runsFor(static_cast<std::size_t>(rows)); ++run) {
            morph::exec::MainThreadExecutor owner;
            Engine engine{source, wideEngine(&owner)};
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
        std::println("| {} | {:.2f} | {:.2f}-{:.2f} | {:.2f} | {:.2f}-{:.2f} |", rows, l.median, l.p10, l.p90,
                     w.median, w.p10, w.p90);
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

// ── Wire: the generic page and the description ──────────────────────────────
// docs/spec/table/table.md, "Benchmarks: wire". Each measurement first REQUIREs its page's cell count, so a writer
// that dropped columns could not pass for a fast one.

// Reflected by glaze, so not in an anonymous namespace.
namespace tbench {

// A typed row of wideColumns(30), member i being column i: what a typed `Page<Row>` of the same table carries. A
// quantity is its amount, as a typed member in the column's unit would be; an empty cell is an absent member.
struct WideRow30 {
    std::optional<std::int64_t> id;
    std::optional<std::int64_t> i001;
    std::optional<std::int64_t> i002;
    std::optional<std::int64_t> i003;
    std::optional<std::int64_t> i004;
    std::optional<std::int64_t> i005;
    std::optional<std::int64_t> i006;
    std::optional<std::int64_t> i007;
    std::optional<std::int64_t> i008;
    std::optional<std::int64_t> i009;
    std::optional<morph::math::Rational> d000;
    std::optional<morph::math::Rational> d001;
    std::optional<morph::math::Rational> d002;
    std::optional<morph::math::Rational> d003;
    std::optional<morph::math::Rational> d004;
    std::optional<morph::math::Rational> d005;
    std::optional<morph::math::Rational> q000;
    std::optional<morph::math::Rational> q001;
    std::optional<morph::math::Rational> q002;
    std::optional<morph::math::Rational> m000;
    std::optional<std::string> t000;
    std::optional<std::string> t001;
    std::optional<std::string> t002;
    std::optional<std::string> t003;
    std::optional<std::string> t004;
    std::optional<std::string> t005;
    std::optional<std::string> t006;
    std::optional<std::int64_t> a000;
    std::optional<std::int64_t> s000;
    std::optional<bool> b000;
};

}  // namespace tbench

namespace {

template <typename T>
void assignCell(std::optional<T>& member, Cell const& cell) {
    if (std::holds_alternative<std::monostate>(cell)) {
        member.reset();
    } else if constexpr (std::is_same_v<T, morph::math::Rational>) {
        if (auto const* const quantity = std::get_if<QuantityCell>(&cell)) {
            member = quantity->amount;
        } else if (auto const* const measure = std::get_if<Measure>(&cell)) {
            member = measure->amount;
        } else {
            member = std::get<morph::math::Rational>(cell);
        }
    } else {
        member = std::get<T>(cell);
    }
}

Page<tbench::WideRow30> typedPage30(bench::WideTableSource const& source, PageRows const& rows) {
    constexpr auto kMembers = glz::reflect<tbench::WideRow30>::size;
    REQUIRE(source.columns().size() == kMembers);
    auto const snapshot = source.snapshot();
    Page<tbench::WideRow30> page;
    page.total = rows.total;
    page.offset = rows.offset;
    for (auto const row : rows.rows) {
        tbench::WideRow30 typed;
        [&]<std::size_t... I>(std::index_sequence<I...>) {
            (assignCell(morph::detail::reflectedMember<I>(typed), snapshot->cell(row, I)), ...);
        }(std::make_index_sequence<kMembers>{});
        page.rows.push_back(std::move(typed));
    }
    return page;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): `firstRows(count, total)` is a test helper whose arguments read differently at each call; a swap would fail the case's own assertion
PageRows firstRows(std::size_t count, std::int64_t total) {
    PageRows rows;
    for (std::size_t i = 0; i < count; ++i) {
        rows.rows.push_back(i);
    }
    rows.total = total;
    return rows;
}

std::size_t cellsOf(GenericPage const& page) {
    std::size_t cells = 0;
    for (auto const& row : page.rows) {
        cells += row.size();
    }
    return cells;
}

std::vector<std::string> columnIds(DataSource const& source, std::size_t first, std::size_t last) {
    std::vector<std::string> out;
    for (auto i = first; i < last; ++i) {
        out.push_back(source.columns()[i].id);
    }
    return out;
}

Spread timed(int runs, std::function<void()> const& body) {
    std::vector<double> samples;
    for (int run = 0; run < runs; ++run) {
        auto const began = Clock::now();
        body();
        samples.push_back(millisSince(began));
    }
    return spreadOf(samples);
}

void printWire(std::string const& id, std::string const& what, std::size_t bytes, Spread const& spread) {
    std::println("| {} | {} | {} | {:.3f} | {:.3f}-{:.3f} |", id, what, bytes, spread.median, spread.p10, spread.p90);
    static_cast<void>(std::fflush(stdout));
}

template <typename T>
std::string jsonOf(T const& value) {
    auto json = glz::write_json(value);
    REQUIRE(json.has_value());
    return std::move(*json);
}

}  // namespace

TEST_CASE("table benchmark: the generic page", "[.benchmark][wire]") {
    constexpr int kRuns = 31;
    bench::WideTableSource const wide{{.rows = 1'000, .columns = 300, .seed = 1}};
    bench::WideTableSource const small{{.rows = 1'000, .columns = 30, .seed = 1}};
    auto const description = describe(bench::wideDescription("wide", 300));
    auto const rows = firstRows(200, 1'000);
    std::println(
        "\nWire: the generic page and the description (bytes; time per call)\n\n| id | measures | bytes | median ms | "
        "p10-p90 ms |\n|---|---|---:|---:|---:|");

    // W1, W2: 200 rows of the 40 columns at positions 100 to 139.
    auto const forty = columnIds(wide, 100, 140);
    auto const page40 = writeGenericPage(rows, wide, forty, wide.units().get());
    REQUIRE(cellsOf(page40) == 8'000);
    auto const body40 = jsonOf(page40);
    auto const envelope40 = morph::wire::encode(morph::wire::makeOk(1, body40)).size();
    printWire("W1", "200 x 40 generic page in an ok envelope", envelope40, Spread{});
    printWire("W2", "encode 200 x 40 from the column store", body40.size(), timed(kRuns, [&] {
                  REQUIRE(jsonOf(writeGenericPage(rows, wide, forty, wide.units().get())).size() == body40.size());
              }));
    printWire("W2", "readGenericPage 200 x 40", body40.size(), timed(kRuns, [&] {
                  auto const read = readGenericPage(body40, description);
                  REQUIRE(read.has_value());
                  REQUIRE(read->rows.size() == 200);
              }));

    // W1, W2 over the bridge: the same page as the `Table.wide` reply, through LocalBackend, and the server's side
    // of it (the table function, the pinned snapshot, the answer and its JSON).
    bench::WideTableModel::configure({.rows = 1'000, .columns = 300, .seed = 1, .loadDelay = {}},
                                     {.rows = 1'000, .columns = 30, .seed = 1, .loadDelay = {}});
    TableQuery query;
    query.page = PageRequest{.offset = 0, .limit = 200};
    query.columns = forty;
    {
        morph::exec::ThreadPoolExecutor pool{2};
        morph::exec::MainThreadExecutor owner;
        morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool), owner};
        morph::bridge::BridgeHandler<bench::WideTableModel> handler{bridge, &owner};
        using WideQuery = QueryTable<&bench::WideTableModel::wide>;
        auto const fetch = [&] {
            std::optional<GenericPage> reply;
            handler.execute(WideQuery{.query = query})
                .then([&reply](GenericPage page) { reply = std::move(page); })
                .onError([](std::exception_ptr const&) {});
            auto const deadline = Clock::now() + std::chrono::seconds{30};
            while (!reply && Clock::now() < deadline) {
                owner.runFor(std::chrono::milliseconds{1});
            }
            REQUIRE(reply.has_value());
            return std::move(*reply);
        };
        auto const reply = fetch();
        REQUIRE(reply.keys.size() == 200);
        REQUIRE(cellsOf(reply) == 8'000);
        auto const replyBody = jsonOf(reply);
        auto const replyEnvelope = morph::wire::encode(morph::wire::makeOk(1, replyBody)).size();
        printWire("W1", "Table.wide reply, 200 x 40, through LocalBackend, in an ok envelope", replyEnvelope,
                  Spread{});
        printWire("W1", "Table.wide round trip through LocalBackend", replyBody.size(),
                  timed(kRuns, [&] { REQUIRE(fetch().keys.size() == 200); }));
        morph::session::Context const session{};
        TableContext const context{.session = session};
        bench::WideTableModel model;
        printWire("W2", "Table.wide server side: serve, pin, answer, encode", replyBody.size(), timed(kRuns, [&] {
                      auto const answer = detail::answerQuery(detail::pinned(model.wide(context)), query);
                      REQUIRE(answer.has_value());
                      REQUIRE(jsonOf(*answer).size() == replyBody.size());
                  }));
    }

    // W5: the same 200 x 30 rows as generic rows and as a typed page.
    auto const thirty = columnIds(small, 0, 30);
    auto const page30 = writeGenericPage(rows, small, thirty, small.units().get());
    REQUIRE(cellsOf(page30) == 6'000);
    auto const generic30 = jsonOf(page30).size();
    auto const typed = typedPage30(small, rows);
    REQUIRE(typed.rows.size() == 200);
    auto const typed30 = jsonOf(typed).size();
    printWire("W5", "200 x 30 generic page, encode", generic30, timed(kRuns, [&] {
                  REQUIRE(jsonOf(writeGenericPage(rows, small, thirty, small.units().get())).size() == generic30);
              }));
    printWire("W5", "200 x 30 typed Page<Row>, encode", typed30,
              timed(kRuns, [&] { REQUIRE(jsonOf(typedPage30(small, rows)).size() == typed30); }));
    std::println("| W5 | generic / typed bytes | {:.3f} | | |",
                 static_cast<double>(generic30) / static_cast<double>(typed30));

    // W6: the 300-column description.
    REQUIRE(description.columns.size() == 300);
    auto const descriptionJson = jsonOf(description);
    printWire("W6", "300-column description, encode", descriptionJson.size(),
              timed(kRuns, [&] { REQUIRE(jsonOf(describe(description)).size() == descriptionJson.size()); }));
    printWire("W6", "300-column description, decode", descriptionJson.size(), timed(kRuns, [&] {
                  TableDescription back;
                  REQUIRE_FALSE(glz::read_json(back, descriptionJson));
                  REQUIRE(back.columns.size() == 300);
              }));

    // W7: 200 rows of all 300 columns.
    auto const all = columnIds(wide, 0, 300);
    auto const page300 = writeGenericPage(rows, wide, all, wide.units().get());
    REQUIRE(cellsOf(page300) == 60'000);
    auto const body300 = jsonOf(page300);
    auto const envelope300 = morph::wire::encode(morph::wire::makeOk(1, body300)).size();
    REQUIRE(envelope300 < morph::wire::kMaxEnvelopeBytes);
    printWire("W7", "200 x 300 generic page in an ok envelope", envelope300, Spread{});
    printWire("W7", "encode 200 x 300 from the column store", body300.size(), timed(15, [&] {
                  REQUIRE(jsonOf(writeGenericPage(rows, wide, all, wide.units().get())).size() == body300.size());
              }));
    printWire("W7", "readGenericPage 200 x 300", body300.size(), timed(15, [&] {
                  auto const read = readGenericPage(body300, description);
                  REQUIRE(read.has_value());
                  REQUIRE(read->rows.size() == 200);
              }));
}

// ── Step 2a: unloaded cells and data levels (S1, S2, S4 to S6) ─────────────

namespace {

// The wide table's recipe columns: ten decimals and ten texts.
std::vector<std::string> recipeColumnsOf(DataSource const& source) {
    std::vector<std::string> out;
    for (auto const& column : source.columns()) {
        if (column.level == "recipe") {
            out.push_back(column.id);
        }
    }
    return out;
}

// Rows of @p column holding `Unloaded`.
std::size_t unloadedIn(DataSource const& source, std::string_view column) {
    auto const index = detail::findColumn(source.columns(), column);
    REQUIRE(index.has_value());
    auto const snapshot = source.snapshot();
    std::size_t count = 0;
    for (std::size_t row = 0; row < snapshot->rowCount(); ++row) {
        count += std::holds_alternative<Unloaded>(snapshot->cell(row, *index)) ? 1U : 0U;
    }
    return count;
}

}  // namespace

TEST_CASE("table benchmark: S1 S2 with unloaded cells", "[.benchmark][table][unloaded]") {
    auto const wide = std::make_shared<bench::WideTableSource>(
        bench::WideTableOptions{.rows = kWideRows, .columns = 300, .seed = 1, .loadDelay = {}});
    auto const recipe = recipeColumnsOf(*wide);
    REQUIRE(recipe.size() == 20);
    auto const partial = bench::withUnloaded(wide, recipe, "recipe", 0.7, 7);
    auto const whole = bench::withUnloaded(wide, recipe, "recipe", 1.0, 7);
    REQUIRE(unloadedIn(*partial, "d000") > 25'000);
    REQUIRE(unloadedIn(*partial, "t001") > 25'000);
    REQUIRE(unloadedIn(*whole, "d000") == 0);
    auto const s1 = wideQuery({{.column = "d000", .dir = kAsc}}, {}, 0);
    auto const s2 = wideQuery({}, filterOn("t001", FilterEntry{.contains = "ka"}), 0);
    {
        auto const page = apply(*partial, s1, wideOptions());
        REQUIRE(page.has_value());
        REQUIRE(page->rows.size() == 200);
        auto const filtered = apply(*partial, s2, wideOptions());
        REQUIRE(filtered.has_value());
        REQUIRE(filtered->undecided > 25'000);
        REQUIRE(filtered->totalKind == TotalKind::AtLeast);
    }
    printHeader("Step 2a: apply over the wide table with the recipe level, 100,000 x 300, in the calling thread");
    auto const time = [&](std::string const& scenario, DataSource const& source, TableQuery const& query) {
        std::vector<double> samples;
        for (int run = 0; run < runsFor(kWideRows); ++run) {
            auto const began = Clock::now();
            auto const page = apply(source, query, wideOptions());
            samples.push_back(millisSince(began));
            REQUIRE(page.has_value());
        }
        printRow(scenario, kWideRows, spreadOf(samples));
    };
    time("S1: sort d000 asc, page of 200, 70% of recipe loaded", *partial, s1);
    time("S1: the same, every row loaded", *whole, s1);
    time("S2: filter t001 contains \"ka\", 70% of recipe loaded", *partial, s2);
    time("S2: the same, every row loaded", *whole, s2);
}

TEST_CASE("table benchmark: S4 levelsNeeded", "[.benchmark][table][unloaded]") {
    auto const columns = bench::wideColumns(300);
    std::vector<ColumnInfo> infos;
    infos.reserve(columns.size());
    for (auto const& column : columns) {
        infos.push_back(column.info);
    }
    TableQuery query;
    // Eight sort keys, two of them on recipe columns; 32 filtered columns, four of them recipe columns.
    for (auto const* const id : {"d000", "i001", "i002", "t001", "i003", "i004", "i005", "i006"}) {
        query.sort.push_back(SortKey{.column = id, .dir = kAsc});
    }
    for (int i = 0; i < 32; ++i) {
        auto const id = i < 4 ? std::format("d{:03}", i + 1) : std::format("i{:03}", i + 7);
        query.filters.columns.emplace(id, ColumnFilter{.include = {FilterEntry{.gt = "0"}}});
    }
    REQUIRE(query.filters.columns.size() == 32);
    auto const checked = validate(query, infos);
    REQUIRE(checked.has_value());
    auto const needed = levelsNeeded(*checked, infos);
    REQUIRE(needed.order == std::vector<std::string>{"recipe"});
    REQUIRE(needed.display == std::vector<std::string>{"recipe"});
    constexpr int kCalls = 1000;
    std::vector<double> samples;
    for (int run = 0; run < 15; ++run) {
        std::size_t sink = 0;
        auto const began = Clock::now();
        for (int call = 0; call < kCalls; ++call) {
            sink += levelsNeeded(*checked, infos).order.size();
        }
        samples.push_back(millisSince(began) * 1000.0 / kCalls);
        REQUIRE(sink == kCalls);
    }
    auto const spread = spreadOf(samples);
    std::println(
        "\nS4: levelsNeeded, 300 columns, 8 sort keys, 32 filtered columns\n\n| scenario | median us | p10-p90 "
        "us |\n|---|---:|---:|\n| S4: one call | {:.2f} | {:.2f}-{:.2f} |",
        spread.median, spread.p10, spread.p90);
    static_cast<void>(std::fflush(stdout));
}

TEST_CASE("table benchmark: S5 S6 arrivals", "[.benchmark][table][unloaded]") {
    auto const wide = std::make_shared<bench::WideTableSource>(
        bench::WideTableOptions{.rows = kWideRows, .columns = 300, .seed = 1, .loadDelay = {}});
    auto const recipe = recipeColumnsOf(*wide);
    printHeader("Step 2a: an engine over the wide table when a level arrives, 100,000 x 300, in the calling thread");

    // S5: sorted on a recipe decimal; 200 rows arrive, then the next 1,000.
    std::vector<double> first;
    std::vector<double> second;
    for (int run = 0; run < runsFor(kWideRows); ++run) {
        auto const source = bench::withUnloaded(wide, recipe, "recipe", 0.7, 7);
        Engine engine{source, wideEngine()};
        require(engine.setSort({{.column = "d000", .dir = kAsc}}));
        auto const waiting = source->unloadedRows();
        REQUIRE(waiting.size() > 25'000);
        for (auto const& [from, count, samples] : {std::tuple{std::size_t{0}, std::size_t{200}, &first},
                                                   std::tuple{std::size_t{200}, std::size_t{1000}, &second}}) {
            std::vector<std::size_t> const arriving(waiting.begin() + static_cast<std::ptrdiff_t>(from),
                                                    waiting.begin() + static_cast<std::ptrdiff_t>(from + count));
            auto const before = engine.stats();
            auto const began = Clock::now();
            source->load(arriving);
            samples->push_back(millisSince(began));
            REQUIRE(engine.stats().repairs == before.repairs + 1);
            REQUIRE(engine.stats().fullSorts == before.fullSorts);
        }
    }
    printRow("S5: sorted on d000, 200 rows arrive", kWideRows, spreadOf(first));
    printRow("S5: then the next 1,000 rows arrive", kWideRows, spreadOf(second));

    // S6: filtered on a recipe decimal with half the rows loaded; 1,000 undecided rows arrive.
    std::vector<double> decided;
    auto const filter = filterOn("d000", FilterEntry{.gt = "0"});
    for (int run = 0; run < runsFor(kWideRows); ++run) {
        auto const source = bench::withUnloaded(wide, recipe, "recipe", 0.5, 7);
        Engine engine{source, wideEngine()};
        require(engine.setFilter(filter));
        REQUIRE(engine.undecidedRowCount() > 45'000);
        auto const waiting = source->unloadedRows();
        std::vector<std::size_t> const arriving(waiting.begin(), waiting.begin() + 1000);
        auto const before = engine.stats();
        auto const began = Clock::now();
        source->load(arriving);
        decided.push_back(millisSince(began));
        REQUIRE(engine.stats().repairs == before.repairs + 1);
        REQUIRE(engine.stats().filterRuns == before.filterRuns);
    }
    printRow("S6: about 50,000 undecided, 1,000 of them decided", kWideRows, spreadOf(decided));
}

// ── Step 2b: partial pages and keyed replies on the wire (W3, W4) ──────────

TEST_CASE("table benchmark: partial pages and keyed replies", "[.benchmark][table][wire]") {
    constexpr int kRuns = 31;
    auto const wide = std::make_shared<bench::WideTableSource>(
        bench::WideTableOptions{.rows = 1'000, .columns = 300, .seed = 1, .loadDelay = {}});
    auto const description = describe(bench::wideDescription("wide", 300));
    std::println(
        "\nWire: partial pages and keyed replies (bytes; time per call)\n\n| id | measures | bytes | median ms | "
        "p10-p90 ms |\n|---|---|---:|---:|---:|");

    // W3: W1's page, 200 rows of the 40 columns at positions 100 to 139, which hold the recipe decimals d000 to
    // d009, with no row's recipe level loaded: every row names one unloaded level.
    auto const named = columnIds(*wide, 100, 140);
    auto const unloaded = bench::withUnloaded(wide, recipeColumnsOf(*wide), "recipe", 0.0, 7);
    auto const rows = firstRows(200, 1'000);
    auto const partial = writeGenericPage(rows, *unloaded, named, wide->units().get());
    REQUIRE(partial.unloadedRows.size() == 200);
    REQUIRE(std::ranges::all_of(partial.unloadedRows, [](UnloadedRow const& row) {
        return row.levels == std::vector<std::string>{"recipe"};
    }));
    auto bare = partial;
    bare.unloadedRows.clear();
    auto const withRows = morph::wire::encode(morph::wire::makeOk(1, jsonOf(partial))).size();
    auto const without = morph::wire::encode(morph::wire::makeOk(1, jsonOf(bare))).size();
    auto const w1 =
        morph::wire::encode(morph::wire::makeOk(1, jsonOf(writeGenericPage(rows, *wide, named, wide->units().get()))))
            .size();
    printWire("W3", "200 x 40 page, every row naming one unloaded level, in an ok envelope", withRows, Spread{});
    printWire("W3", "the same page without its unloadedRows", without, Spread{});
    std::println("| W3 | unloadedRows bytes {} as a share of W1's {} | {:.4f} | | |", withRows - without, w1,
                 static_cast<double>(withRows - without) / static_cast<double>(w1));

    // W4: a keyed reply of 20 keys spread over the view and W1's 40 columns.
    TableQuery keyed;
    keyed.columns = named;
    keyed.keys.emplace();
    for (std::int64_t i = 0; i < 20; ++i) {
        keyed.keys->emplace_back(1 + (i * 50));
    }
    auto const keyedRows = apply(*wide, keyed, wideOptions());
    REQUIRE(keyedRows.has_value());
    REQUIRE(keyedRows->rows.size() == 20);
    REQUIRE(keyedRows->indices.size() == 20);
    auto const keyedPage = writeGenericPage(*keyedRows, *wide, *keyed.columns, wide->units().get());
    REQUIRE(cellsOf(keyedPage) == 800);
    auto const keyedBody = jsonOf(keyedPage);
    printWire("W4", "keyed reply, 20 x 40, in an ok envelope",
              morph::wire::encode(morph::wire::makeOk(1, keyedBody)).size(), Spread{});
    printWire("W4", "encode the keyed reply from the column store", keyedBody.size(), timed(kRuns, [&] {
                  REQUIRE(jsonOf(writeGenericPage(*keyedRows, *wide, *keyed.columns, wide->units().get())).size() ==
                          keyedBody.size());
              }));
    printWire("W4", "readGenericPage of the keyed reply", keyedBody.size(), timed(kRuns, [&] {
                  auto const read = readGenericPage(keyedBody, description);
                  REQUIRE(read.has_value());
                  REQUIRE(read->indices.size() == 20);
              }));
}

// ── The server level cache (L1-L3, L6) ──────────────────────────────────────

namespace {

using LevelValues = std::vector<std::pair<RowId, std::vector<Cell>>>;

// Twenty cells as the recipe level holds them: ten decimals and ten texts of 4 to 60 characters.
std::vector<Cell> recipeLikeValue(std::int64_t key) {
    std::vector<Cell> out;
    out.reserve(20);
    for (std::int64_t column = 0; column < 10; ++column) {
        out.emplace_back(tabletest::dec(std::to_string(((key * 31) + column) % 100000) + ".25"));
    }
    for (std::int64_t column = 0; column < 10; ++column) {
        out.emplace_back(std::string(static_cast<std::size_t>(4 + ((key + column) % 57)), 'a'));
    }
    return out;
}

LevelValues recipeLikeValues(std::int64_t first, std::int64_t last) {
    LevelValues out;
    out.reserve(static_cast<std::size_t>(last - first));
    for (auto key = first; key < last; ++key) {
        // Built in place: moving a temporary RowId into the pair makes GCC 15 at -O3 report its string alternative
        // as maybe-uninitialized.
        out.emplace_back(std::piecewise_construct, std::forward_as_tuple(std::in_place_type<std::int64_t>, key),
                         std::forward_as_tuple(recipeLikeValue(key)));
    }
    return out;
}

std::vector<RowId> rowKeys(std::int64_t first, std::int64_t last) {
    std::vector<RowId> out;
    for (auto key = first; key < last; ++key) {
        out.emplace_back(key);
    }
    return out;
}

// A recipe-shaped cache whose loads run where `loads` runs them, the loader returning values at once.
std::shared_ptr<CellLevelCache> benchCache(morph::exec::IExecutor* loads) {
    LevelCacheOptions options;
    options.records = {"Bench.rows"};
    options.level = "recipe";
    options.columns = bench::recipeColumns();
    options.loads = loads;
    return std::make_shared<CellLevelCache>(std::move(options), [](std::span<RowId const> keys) {
        LevelValues out;
        for (auto const& key : keys) {
            out.emplace_back(key, recipeLikeValue(std::get<std::int64_t>(key)));
        }
        return out;
    });
}

morph::effects::Changes keyedChange(std::string record, std::int64_t count) {
    auto const keys = rowKeys(0, count);
    return morph::effects::Changes{.records = {{.record = std::move(record), .keys = changeKeys(keys)}},
                                   .instances = {}};
}

}  // namespace

// Mutation: a snapshot holding a shared lock that stores take exclusively (the calls return after the reader).
TEST_CASE("level cache: invalidation does not wait for a reader", "[table][levels][threads]") {
    auto const cache = benchCache(nullptr);
    morph::table::detail::LevelCacheAccess::store(*cache, recipeLikeValues(0, 5'000));
    // The reader holds its snapshot until the calls have returned, or for ten seconds when they wait for it.
    std::atomic<bool> released{false};
    std::latch holding{1};
    std::promise<void> returned;
    std::thread reader{[&, done = returned.get_future()] {
        auto const snapshot = cache->snapshot();
        holding.count_down();
        static_cast<void>(done.wait_for(std::chrono::seconds{10}));
        released = true;
    }};
    holding.wait();
    cache->invalidate(rowKeys(0, 1'000));
    cache->changed(keyedChange("Bench.rows", 4'096));
    CHECK_FALSE(released.load());
    returned.set_value();
    reader.join();
    CHECK(cache->stats().invalidatedKeys == 4'096);
}

// Mutation: a store copying every shard (256 copies for a batch of 64 keys).
TEST_CASE("level cache: a batch copies only the shards it touches", "[table][levels]") {
    tabletest::ManualExecutor loads;
    auto const cache = benchCache(&loads);
    morph::table::detail::LevelCacheAccess::store(*cache, recipeLikeValues(0, 10'000));
    auto const before = morph::table::detail::LevelCacheAccess::shardCopies(*cache);
    REQUIRE(cache->want(rowKeys(10'000, 10'064), LevelUse::Display));
    tabletest::drain(loads);
    REQUIRE(cache->stats().loadedKeys == 64);
    CHECK(morph::table::detail::LevelCacheAccess::shardCopies(*cache) - before <= 64);
}

TEST_CASE("table benchmark: L1 L2 L3 L6 the level cache", "[.benchmark][levels]") {
    constexpr int kRuns = 15;
    auto const timed = [](int runs, std::function<void()> const& body) {
        std::vector<double> samples;
        for (int run = 0; run < runs; ++run) {
            auto const began = Clock::now();
            body();
            samples.push_back(std::chrono::duration<double, std::milli>(Clock::now() - began).count());
        }
        return spreadOf(std::move(samples));
    };
    std::println("\nThe server level cache\n\n| Id | scenario | median ms | p10-p90 ms |\n|---|---|---:|---:|");
    auto const print = [](char const* id, char const* scenario, Spread const& spread) {
        std::println("| {} | {} | {:.3f} | {:.3f}-{:.3f} |", id, scenario, spread.median, spread.p10, spread.p90);
        static_cast<void>(std::fflush(stdout));
    };

    auto const bytesBefore = bench::processBytes();
    auto const cache = benchCache(nullptr);
    morph::table::detail::LevelCacheAccess::store(*cache, recipeLikeValues(1, 100'001));
    auto const bytesAfter = bench::processBytes();
    REQUIRE(cache->stats().bytes > 0);

    // L1: a page's hit path, and an order level's 100,000 reads.
    auto const page = rowKeys(1, 201);
    auto const all = rowKeys(1, 100'001);
    std::vector<Cell> buffer(20);
    print("L1", "snapshot + 200 reads", timed(kRuns * 4, [&] {
              auto const snapshot = cache->snapshot();
              for (auto const& key : page) {
                  REQUIRE(snapshot->read(key, buffer) == LevelRead::Loaded);
              }
          }));
    print("L1", "100,000 reads", timed(kRuns, [&] {
              auto const snapshot = cache->snapshot();
              for (auto const& key : all) {
                  static_cast<void>(snapshot->read(key, buffer));
              }
          }));

    // L2: what a row costs.
    std::println("| L2 | counted bytes per row at twenty columns | {} | |", cache->stats().bytes / 100'000);
    std::println("| L2 | counted bytes for 100,000 rows | {} | |", cache->stats().bytes);
    std::println("| L2 | process bytes for 100,000 rows | {} | |", bytesAfter - std::min(bytesAfter, bytesBefore));

    // L3: invalidation and the observer call, alone and while a reader holds a snapshot for 40 ms.
    auto const invalidation = rowKeys(1, 1'001);
    auto const change = keyedChange("Bench.rows", 4'096);
    auto const refill = [&] { morph::table::detail::LevelCacheAccess::store(*cache, recipeLikeValues(1, 4'097)); };
    auto const l3 = [&](bool withReader) {
        std::vector<double> invalidate;
        std::vector<double> observe;
        for (int run = 0; run < kRuns; ++run) {
            refill();
            std::latch holding{1};
            std::thread reader;
            if (withReader) {
                reader = std::thread{[&] {
                    auto const snapshot = cache->snapshot();
                    holding.count_down();
                    std::this_thread::sleep_for(std::chrono::milliseconds{40});
                }};
                holding.wait();
            }
            auto began = Clock::now();
            cache->invalidate(invalidation);
            invalidate.push_back(std::chrono::duration<double, std::milli>(Clock::now() - began).count());
            refill();
            began = Clock::now();
            cache->changed(change);
            observe.push_back(std::chrono::duration<double, std::milli>(Clock::now() - began).count());
            if (reader.joinable()) {
                reader.join();
            }
        }
        print("L3", withReader ? "invalidate 1,000 keys, a reader holding a snapshot" : "invalidate 1,000 keys",
              spreadOf(invalidate));
        print("L3",
              withReader ? "observer call, 4,096 keys, a reader holding a snapshot" : "observer call, 4,096 keys",
              spreadOf(observe));
    };
    l3(false);
    l3(true);

    // L6: loader return to announce, per batch of 64, at 100,000 held rows.
    tabletest::ManualExecutor loads;
    LevelCacheOptions options;
    options.records = {"Bench.rows"};
    options.level = "recipe";
    options.columns = bench::recipeColumns();
    options.loads = &loads;
    Clock::time_point returned;
    std::vector<double> storeToAnnounce;
    CellLevelCache timedCache{std::move(options), [&returned](std::span<RowId const> keys) {
                                  LevelValues out;
                                  for (auto const& key : keys) {
                                      out.emplace_back(key, recipeLikeValue(std::get<std::int64_t>(key)));
                                  }
                                  returned = Clock::now();
                                  return out;
                              }};
    timedCache.bind([&](std::string_view, std::optional<std::vector<glz::raw_json>> const&) {
        storeToAnnounce.push_back(std::chrono::duration<double, std::milli>(Clock::now() - returned).count());
    });
    morph::table::detail::LevelCacheAccess::store(timedCache, recipeLikeValues(1, 100'001));
    auto const copiesBefore = morph::table::detail::LevelCacheAccess::shardCopies(timedCache);
    for (int run = 0; run < kRuns; ++run) {
        auto const first = 200'000 + (std::int64_t{run} * 64);
        REQUIRE(timedCache.want(rowKeys(first, first + 64), LevelUse::Display));
        tabletest::drain(loads);
    }
    print("L6", "loader return to announce, batch of 64 at 100,000 rows", spreadOf(storeToAnnounce));
    std::println("| L6 | shards copied for {} keys stored | {} | |", kRuns * 64,
                 morph::table::detail::LevelCacheAccess::shardCopies(timedCache) - copiesBefore);
}

// ── The view cache (S7, S9) ─────────────────────────────────────────────────

TEST_CASE("table benchmark: S7 S9 the view cache", "[.benchmark][views]") {
    auto const source = std::make_shared<bench::WideTableSource>(
        bench::WideTableOptions{.rows = 100'000, .columns = 300, .seed = 1, .loadDelay = {}});
    auto description = morph::table::describe(bench::wideDescription("wide", 300));
    description.capabilities.keys = true;
    // Every column at the base level: the source holds the recipe columns' cells itself.
    for (auto& column : description.columns) {
        column.info.level.clear();
    }
    description.levels.clear();
    auto const served = detail::pinned(ServedTable{.description = morph::table::describe(std::move(description)),
                                                   .source = source,
                                                   .levels = {},
                                                   .mayRead = {},
                                                   .scope = {},
                                                   .services = wideOptions().services,
                                                   .limits = {}});
    ViewCache views;
    detail::AnswerContext const context{.views = &views, .record = "WideTableModel.wide", .rowScoped = false};
    TableQuery query;
    query.sort = {SortKey{.column = "i001", .dir = kAsc}};
    query.columns = std::vector<std::string>{"id", "i001", "t001"};
    std::println("\nThe view cache, 100,000 x 300\n\n| Id | scenario | ms |\n|---|---|---:|");
    std::vector<double> later;
    for (int request = 0; request < 10; ++request) {
        query.page = PageRequest{.offset = static_cast<std::int64_t>(request) * 200, .limit = 200};
        auto const began = Clock::now();
        REQUIRE(detail::answerQuery(served, query, context).has_value());
        auto const ms = std::chrono::duration<double, std::milli>(Clock::now() - began).count();
        if (request == 0) {
            std::println("| S7 | request 1, which sorts | {:.3f} |", ms);
        } else {
            later.push_back(ms);
        }
    }
    auto const spread = spreadOf(later);
    std::println("| S7 | requests 2 to 10, median (p10-p90) | {:.3f} ({:.3f}-{:.3f}) |", spread.median, spread.p10,
                 spread.p90);
    std::println("| S7 | views computed for ten requests | {} |", views.stats().computations);
    auto keyed = query;
    keyed.page = PageRequest{};
    keyed.keys.emplace();
    for (std::int64_t i = 0; i < 20; ++i) {
        keyed.keys->emplace_back(1 + (i * 4'999));
    }
    for (int run = 0; run < 2; ++run) {
        auto const began = Clock::now();
        auto const page = detail::answerQuery(served, keyed, context);
        auto const ms = std::chrono::duration<double, std::milli>(Clock::now() - began).count();
        INFO((page.has_value() ? std::string{} : page.error().message));
        REQUIRE(page.has_value());
        REQUIRE(page->keys.size() == 20);
        std::println("| S9 | keyed query of 20 keys, {} | {:.3f} |", run == 0 ? "first" : "next", ms);
    }
    std::println("| S9 | views computed after both keyed queries | {} |", views.stats().computations);
    static_cast<void>(std::fflush(stdout));
}

// ── The window (K1-K3) ──────────────────────────────────────────────────────

namespace {

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): `windowPage(offset, rows, columns)` is a test helper whose arguments read differently at each call; a swap would fail the case's own assertion
GenericPage windowPage(std::int64_t offset, std::int64_t rows, std::vector<std::string> const& columns) {
    GenericPage page;
    page.columns = columns;
    page.offset = offset;
    page.total = 100'000;
    for (std::int64_t row = 0; row < rows; ++row) {
        page.keys.emplace_back(offset + row);
        page.rows.emplace_back(columns.size(), Cell{offset + row});
    }
    return page;
}

std::vector<std::string> windowColumns(std::size_t count) {
    std::vector<std::string> out;
    for (std::size_t column = 0; column < count; ++column) {
        // NOLINTNEXTLINE(performance-inefficient-vector-operation): `out` is a fixture list built once; sizing it first would not change what the case checks
        out.push_back(std::format("c{:03}", column));
    }
    return out;
}

}  // namespace

namespace {

// D1-D4's table: 100,000 rows; `t` a text of 50,000 distinct values of 3 to 10 lowercase letters, `o1` an integer
// 0..99, `o2` an integer 0..99 at level "slow", unloaded in 30% of the rows. From std::mt19937{7}.
std::shared_ptr<VectorSource> distinctBenchSource() {
    // NOLINTNEXTLINE(bugprone-random-generator-seed, cert-msc32-c, cert-msc51-cpp): the fixed seed 7 replays the same sequence on every run, so a failing case reproduces
    std::mt19937 rng{7};
    std::vector<std::string> words;
    std::set<std::string> seen;
    while (words.size() < 50'000) {
        std::string word(3 + (rng() % 8), 'a');
        for (auto& letter : word) {
            letter = static_cast<char>('a' + (rng() % 26));
        }
        if (seen.insert(word).second) {
            words.push_back(std::move(word));
        }
    }
    std::vector<std::vector<Cell>> rows;
    rows.reserve(100'000);
    for (std::size_t row = 0; row < 100'000; ++row) {
        auto const& word = words[row < words.size() ? row : rng() % words.size()];
        rows.push_back({Cell{word}, Cell{static_cast<std::int64_t>(rng() % 100)},
                        rng() % 10 < 3 ? Cell{Unloaded{}} : Cell{static_cast<std::int64_t>(rng() % 100)}});
    }
    return tabletest::makeSource({tabletest::col("t", ColumnKind::Text), tabletest::col("o1", ColumnKind::Integer),
                                  tabletest::levelCol("o2", ColumnKind::Integer, "slow")},
                                 std::move(rows));
}

Spread timedRuns(int runs, std::function<void()> const& body) {
    std::vector<double> samples;
    for (int run = 0; run < runs; ++run) {
        auto const began = Clock::now();
        body();
        samples.push_back(std::chrono::duration<double, std::milli>(Clock::now() - began).count());
    }
    return spreadOf(samples);
}

}  // namespace

TEST_CASE("table benchmark: K1 K2 K3 the window", "[.benchmark][window]") {
    auto const wide = windowColumns(300);
    std::println("\nThe window\n\n| Id | scenario | median ms (p10-p90) |\n|---|---|---:|");
    std::vector<double> accepts;
    for (int run = 0; run < 31; ++run) {
        PageWindow<GenericRow> window{200, 8, 1000, 300};
        auto const fetches = window.visible(0, 50, wide);
        REQUIRE_FALSE(fetches.empty());
        auto page = windowPage(0, 200, wide);
        auto const began = Clock::now();
        bool const accepted = window.accept(fetches[0], std::move(page));
        REQUIRE(accepted);
        accepts.push_back(std::chrono::duration<double, std::milli>(Clock::now() - began).count());
    }
    auto spread = spreadOf(accepts);
    std::println("| K1 | accept of a 200 x 300 page | {:.3f} ({:.3f}-{:.3f}) |", spread.median, spread.p10,
                 spread.p90);

    auto const narrow = windowColumns(1);
    PageWindow<GenericRow> held{200, 8};
    for (std::int64_t page = 0; page < 16; page += 2) {
        for (auto const& fetch : held.visible(page * 200, 1, narrow)) {
            if (fetch.request.offset == page * 200) {
                REQUIRE(held.accept(fetch, windowPage(fetch.request.offset, 200, narrow)));
            }
        }
    }
    std::vector<RowId> event;
    for (std::int64_t key = 50'000; key < 50'900; ++key) {
        event.emplace_back(key);
    }
    std::vector<double> refreshes;
    RefreshPlan plan;
    for (int run = 0; run < 31; ++run) {
        auto const began = Clock::now();
        plan = held.refresh(event);
        refreshes.push_back(std::chrono::duration<double, std::milli>(Clock::now() - began).count());
    }
    REQUIRE(plan.rows.keys.size() == 908);
    spread = spreadOf(refreshes);
    std::println("| K2 | refresh of 900 keys over 1,600 held rows in eight runs | {:.3f} ({:.3f}-{:.3f}) |",
                 spread.median, spread.p10, spread.p90);

    std::vector<double> patches;
    for (int run = 0; run < 31; ++run) {
        std::vector<RowId> keys;
        for (std::int64_t key = 0; key < 200; ++key) {
            // NOLINTNEXTLINE(performance-inefficient-vector-operation): `keys` is a fixture list built once; sizing it first would not change what the case checks
            keys.emplace_back(key);
        }
        auto const rows = held.refresh(keys);
        REQUIRE(rows.kind == Refresh::Rows);
        // The 200 keys and the seven other runs' probes, each at its own view row.
        GenericPage reply;
        reply.columns = narrow;
        reply.total = 100'000;
        for (auto const& key : rows.rows.keys) {
            auto const index = std::get<std::int64_t>(key);
            reply.keys.push_back(key);
            reply.rows.emplace_back(1, Cell{index});
            reply.indices.push_back(index);
        }
        auto const began = Clock::now();
        auto const replaced = held.patch(rows.rows, std::move(reply));
        patches.push_back(std::chrono::duration<double, std::milli>(Clock::now() - began).count());
        REQUIRE(replaced.size() == 207);
    }
    spread = spreadOf(patches);
    std::println("| K3 | patch of 200 rows and seven probes | {:.3f} ({:.3f}-{:.3f}) |", spread.median, spread.p10,
                 spread.p90);
    static_cast<void>(std::fflush(stdout));
}

// ── Many clients, in process (C1-C3) ────────────────────────────────────────

TEST_CASE("table benchmark: C1 C2 C3 many requests through an in-process server", "[.benchmark][views]") {
    bench::WideTableModel::recipeGate().release();
    bench::WideTableModel::configure({.rows = 100'000, .columns = 300, .seed = 7, .loadDelay = {}},
                                     {.rows = 1'000, .columns = 30, .seed = 7, .loadDelay = {}});
    constexpr std::size_t kViewBytes = 4UZ * 100'000;
    // Ten base-column views and one view sorted by a level, which also keeps its 100,000 integer order keys: about
    // twenty base-column views once the level-sorted one is evicted.
    constexpr std::size_t kBound = (10 * kViewBytes) + sizeof(std::vector<RowId>) + (100'000 * sizeof(RowId));
    morph::exec::ThreadPoolExecutor pool{4};
    morph::backend::ServerConfig config;
    config.tableWork = std::make_shared<morph::exec::ThreadPoolExecutor>(8);
    config.views.maxBytes = kBound;
    auto const server = std::make_shared<morph::backend::RemoteServer>(pool, std::move(config));
    server->attach(bench::WideTableModel::recipeCache());
    // An instance per request of C1: one instance runs its executes one at a time.
    std::vector<std::uint64_t> instances;
    for (int instance = 0; instance < 50; ++instance) {
        auto const registered = morph::wire::decode(
            server->handleInline(morph::wire::encode(morph::wire::makeRegister(std::string{bench::kWideModelType}))));
        REQUIRE(registered.kind == "ok");
        instances.push_back(registered.modelId);
    }
    std::uint64_t callId = 1;
    auto const send = [&](std::string body, std::size_t instance = 0) {
        morph::wire::Envelope execute;
        execute.kind = "execute";
        execute.callId = callId++;
        execute.modelId = instances[instance];
        execute.modelType = std::string{bench::kWideModelType};
        execute.actionType = "Table.wide";
        execute.body = std::move(body);
        auto waiter = std::make_shared<morph::testing::WaitReply>();
        server->handle(morph::wire::encode(execute), [waiter](std::string const& reply) { (*waiter)(reply); });
        return waiter;
    };
    auto const settle = [](std::shared_ptr<morph::testing::WaitReply> const& waiter) {
        REQUIRE(waiter->await(std::chrono::seconds{120}));
        REQUIRE(waiter->env.kind == "ok");
    };
    std::println(
        "\nMany requests through an in-process server, 100,000 x 300\n\n| Id | scenario | figure |\n|---|---|---|");
    std::string const sorted = R"({"sort":[{"column":"d000","dir":"asc"}],"columns":["id","d000"],)"
                               R"("page":{"offset":0,"limit":50}})";
    settle(send(sorted));  // the first view, before the arrival
    // C1: an arrival on the order level, then fifty concurrent refetches of the view.
    auto const cache = bench::WideTableModel::recipeCache();
    std::vector<Cell> cells(20);
    REQUIRE(cache->snapshot()->read(RowId{std::int64_t{1}}, cells) == LevelRead::Loaded);
    morph::table::detail::LevelCacheAccess::store(
        *cache, std::vector<std::pair<RowId, std::vector<Cell>>>{{RowId{std::int64_t{1}}, cells}});
    auto const before = server->viewStats();
    auto const fitsBefore = morph::table::detail::LevelCacheAccess::fits(*cache);
    auto const began = Clock::now();
    std::vector<std::shared_ptr<morph::testing::WaitReply>> waiters;
    for (std::size_t request = 0; request < 50; ++request) {
        // NOLINTNEXTLINE(performance-inefficient-vector-operation): `waiters` is a fixture list built once; sizing it first would not change what the case checks
        waiters.push_back(send(sorted, request));
    }
    for (auto const& waiter : waiters) {
        settle(waiter);
    }
    auto const c1 = std::chrono::duration<double, std::milli>(Clock::now() - began).count();
    auto const after = server->viewStats();
    std::println(
        "| C1 | fifty concurrent refetches after an arrival, send to last reply | {:.1f} ms; {} computed, {} waited; "
        "{} order wants fitted |",
        c1, after.computations - before.computations, after.waits - before.waits,
        morph::table::detail::LevelCacheAccess::fits(*cache) - fitsBefore);
    // C2: distinct sorts, one after another.
    auto const c2Began = Clock::now();
    for (int column = 1; column <= 20; ++column) {
        settle(send(std::format(R"({{"sort":[{{"column":"i{:03}","dir":"asc"}}],"columns":["id"],)"
                                R"("page":{{"offset":0,"limit":10}}}})",
                                column)));
    }
    auto const seconds = std::chrono::duration<double>(Clock::now() - c2Began).count();
    std::println("| C2 | views computed per second, twenty sorts one after another | {:.2f} |", 20.0 / seconds);
    // C3: fifty sorts in all against the bound: about twenty base-column views, so fewer evictions than ten would
    // give.
    for (int column = 21; column <= 50; ++column) {
        settle(send(std::format(R"({{"sort":[{{"column":"i{:03}","dir":"asc"}}],"columns":["id"],)"
                                R"("page":{{"offset":0,"limit":10}}}})",
                                column)));
    }
    auto const stats = server->viewStats();
    std::println("| C3 | after fifty sorts | {} bytes held of a {} bound, {} evictions |", stats.bytes, kBound,
                 stats.evictions);
    CHECK(stats.bytes <= kBound);
    std::println("| C1 | the server strand's longest task | not measured in process |");
    static_cast<void>(std::fflush(stdout));
}

TEST_CASE("table benchmark: C1 where a request's time goes", "[.benchmark][views]") {
    bench::WideTableModel::recipeGate().release();
    bench::WideTableModel::configure({.rows = 100'000, .columns = 300, .seed = 7, .loadDelay = {}},
                                     {.rows = 1'000, .columns = 30, .seed = 7, .loadDelay = {}});
    std::println("\nOne request's parts, 100,000 x 300\n\n| part | median ms | p10-p90 ms |\n|---|---:|---:|");
    auto const report = [](char const* part, std::vector<double> samples) {
        auto const spread = spreadOf(std::move(samples));
        std::println("| {} | {:.3f} | {:.3f}-{:.3f} |", part, spread.median, spread.p10, spread.p90);
        static_cast<void>(std::fflush(stdout));
    };
    auto const time = [](std::function<void()> const& body) {
        auto const began = Clock::now();
        body();
        return std::chrono::duration<double, std::milli>(Clock::now() - began).count();
    };
    bench::WideTableModel model;
    morph::session::Context const session{};
    TableContext const context{.session = session};
    std::vector<double> serve;
    std::vector<double> want;
    std::vector<double> sortedOnLevel;
    std::vector<double> sortedOnBase;
    ViewCache views;
    auto const answer = [&](std::string column) {
        auto const served = detail::pinned(detail::serve<&bench::WideTableModel::wide>(model, context));
        TableQuery query;
        query.sort = {SortKey{.column = std::move(column), .dir = kAsc}};
        query.columns = std::vector<std::string>{"id", "d000"};
        query.page = PageRequest{.offset = 0, .limit = 50};
        detail::AnswerContext const answerContext{
            .views = &views, .record = "WideTableModel.wide", .rowScoped = false};
        REQUIRE(detail::answerQuery(served, query, answerContext).has_value());
    };
    answer("d000");
    answer("i001");
    auto const cache = bench::WideTableModel::recipeCache();
    std::vector<RowId> keys;
    for (std::int64_t key = 1; key <= 100'000; ++key) {
        keys.emplace_back(key);
    }
    for (int run = 0; run < 9; ++run) {
        serve.push_back(time(
            [&] { static_cast<void>(detail::pinned(detail::serve<&bench::WideTableModel::wide>(model, context))); }));
        want.push_back(time([&] { REQUIRE(cache->want(keys, LevelUse::Order)); }));
        sortedOnLevel.push_back(time([&] { answer("d000"); }));
        sortedOnBase.push_back(time([&] { answer("i001"); }));
    }
    report("the table function, its description and the pinned snapshot", serve);
    report("an order want of the 100,000 keys of a held level", want);
    report("answerQuery from a cached view, sorted on a recipe column (an order want)", sortedOnLevel);
    report("answerQuery from a cached view, sorted on a base column (no order want)", sortedOnBase);
    std::println("| views computed | {} | |", views.stats().computations);
}

TEST_CASE("table benchmark: distinct values and value sets", "[.benchmark][table][distinct]") {
    auto const source = distinctBenchSource();
    constexpr int kRuns = 9;
    printHeader("D1-D4: distinct values and value sets");

    Engine sorted{source};
    require(sorted.setSort({SortKey{.column = "t", .dir = kAsc}}));
    REQUIRE(sorted.stats().keyBuilds == 1);
    auto const first = sorted.distinctValues({.column = "t"});
    REQUIRE(first);
    REQUIRE(first->values.size() == 200);
    REQUIRE(first->distinct == 50'000);
    printRow("D1 distinctValues, 50,000 distinct, first 200, keys built", 100'000,
             timedRuns(kRuns, [&] { std::ignore = (sorted.distinctValues({.column = "t"})); }));

    auto const searched = sorted.distinctValues({.column = "t", .search = "abc"});
    REQUIRE(searched);
    REQUIRE(searched->distinct > 0);
    printRow("D2 the same, search \"abc\"", 100'000,
             timedRuns(kRuns, [&] { std::ignore = (sorted.distinctValues({.column = "t", .search = "abc"})); }));

    Engine filtered{source};
    require(filtered.setSort({SortKey{.column = "t", .dir = kAsc}}));
    FilterSpec others;
    others.columns.emplace("o1",
                           ColumnFilter{.combine = Combine::Any, .include = {FilterEntry{.ge = "20"}}, .exclude = {}});
    others.columns.emplace("o2",
                           ColumnFilter{.combine = Combine::Any, .include = {FilterEntry{.lt = "80"}}, .exclude = {}});
    require(filtered.setFilter(others));
    auto const partial = filtered.distinctValues({.column = "t"});
    REQUIRE(partial);
    REQUIRE(partial->partial);
    printRow("D3 the same, filters on o1 and on o2 (30% unloaded)", 100'000,
             timedRuns(kRuns, [&] { std::ignore = (filtered.distinctValues({.column = "t"})); }));

    std::vector<std::string> members;
    auto const snapshot = source->snapshot();
    for (std::size_t row = 0; row < 10'000; ++row) {
        // NOLINTNEXTLINE(performance-inefficient-vector-operation): `members` is a fixture list built once; sizing it first would not change what the case checks
        members.push_back(std::get<std::string>(snapshot->cell(row * 5, 0)));
    }
    auto const columns = source->columns();
    Services const services;
    auto keys = KeyColumn::forColumn(columns[0], services, snapshot->rowCount());
    buildKeys(*snapshot, 0, columns[0], services, 0, snapshot->rowCount(), keys);
    std::vector<KeyColumn const*> const pointers{&keys, nullptr, nullptr};
    auto const spec = filterOn("t", FilterEntry{.oneOf = members});
    printRow("D4 compile a 10,000-value oneOf", 100'000,
             timedRuns(kRuns, [&] { std::ignore = (compileFilter(spec, columns, services)); }));
    auto const compiled = compileFilter(spec, columns, services);
    REQUIRE(compiled);
    std::size_t passed = 0;
    printRow("D4 filter 100,000 rows with it", 100'000, timedRuns(kRuns, [&] {
                 std::vector<std::uint32_t> rows(snapshot->rowCount());
                 for (std::size_t row = 0; row < rows.size(); ++row) {
                     rows[row] = static_cast<std::uint32_t>(row);
                 }
                 FilterRun run{*compiled, pointers, std::move(rows)};
                 static_cast<void>(run.run(kNoDeadline, ::core::async::StopToken{}));
                 passed = std::move(run).take().size();
             }));
    REQUIRE(passed >= 10'000);
}

namespace {

// G1-G7's table: 100,000 rows; a group of 3 (h0..h2) and a group of 16 (k0..k15) text columns, each cell one of
// "v0".."v31" from std::mt19937{7}; every column at level "slow", so `unloaded` can make a third of them Unloaded.
std::shared_ptr<VectorSource> groupBenchSource(bool unloaded) {
    // NOLINTNEXTLINE(bugprone-random-generator-seed, cert-msc32-c, cert-msc51-cpp): the fixed seed 7 replays the same sequence on every run, so a failing case reproduces
    std::mt19937 rng{7};
    std::vector<ColumnInfo> columns;
    std::vector<std::string> three;
    std::vector<std::string> sixteen;
    for (int c = 0; c < 3; ++c) {
        three.push_back(std::format("h{}", c));
        columns.push_back(tabletest::levelCol(three.back(), ColumnKind::Text, "slow"));
    }
    for (int c = 0; c < 16; ++c) {
        sixteen.push_back(std::format("k{}", c));
        columns.push_back(tabletest::levelCol(sixteen.back(), ColumnKind::Text, "slow"));
    }
    std::vector<std::string> values;
    for (int v = 0; v < 32; ++v) {
        // NOLINTNEXTLINE(performance-inefficient-vector-operation): `values` is a fixture list built once; sizing it first would not change what the case checks
        values.push_back(std::format("v{}", v));
    }
    std::vector<std::vector<Cell>> rows(100'000);
    for (auto& row : rows) {
        row.reserve(columns.size());
        for (std::size_t c = 0; c < columns.size(); ++c) {
            if (unloaded && rng() % 3 == 0) {
                row.emplace_back(Unloaded{});
            } else {
                row.emplace_back(values[rng() % values.size()]);
            }
        }
    }
    auto source = tabletest::makeSource(std::move(columns), std::move(rows));
    source->setGroups({ColumnGroupInfo{.id = "three", .label = "Three", .columns = three},
                       ColumnGroupInfo{.id = "sixteen", .label = "Sixteen", .columns = sixteen}});
    return source;
}

// The members "v0".."v{n-1}", then "w0".. to make up `count` (values no cell holds).
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): `setOf(held, count)` is a test helper whose arguments read differently at each call; a swap would fail the case's own assertion
std::vector<std::string> setOf(int held, int count) {
    std::vector<std::string> out;
    for (int v = 0; v < count; ++v) {
        // NOLINTNEXTLINE(performance-inefficient-vector-operation): `out` is a fixture list built once; sizing it first would not change what the case checks
        out.push_back(v < held ? std::format("v{}", v) : std::format("w{}", v));
    }
    return out;
}

}  // namespace

TEST_CASE("table benchmark: column groups", "[.benchmark][table][groups]") {
    constexpr int kRuns = 9;
    printHeader("G1-G4, G6, G7: column groups");
    for (bool const unloaded : {false, true}) {
        auto const source = groupBenchSource(unloaded);
        // Keys built: every group filter below reads them from an engine that already holds them.
        Engine engine{source};
        FilterSpec warm;
        warm.groups.push_back({.group = "three", .include = {FilterEntry{.eq = "v0"}}});
        warm.groups.push_back({.group = "sixteen", .include = {FilterEntry{.eq = "v0"}}});
        require(engine.setFilter(warm));
        auto const time = [&](std::string const& scenario, GroupFilter const& group) {
            FilterSpec spec;
            spec.groups.push_back(group);
            require(engine.setFilter(spec));
            REQUIRE((engine.undecidedRowCount() > 0) == unloaded);
            std::vector<double> samples;
            for (int run = 0; run < kRuns; ++run) {
                require(engine.setFilter(FilterSpec{}));
                auto const began = Clock::now();
                require(engine.setFilter(spec));
                samples.push_back(std::chrono::duration<double, std::milli>(Clock::now() - began).count());
            }
            auto const spread = spreadOf(samples);
            printRow(std::format("{}{}", scenario, unloaded ? " (a third unloaded)" : ""), 100'000, spread);
        };
        for (auto const* const id : {"three", "sixteen"}) {
            std::string const name = id;
            std::vector<FilterEntry> together;
            auto const k = name == "three" ? 3 : 16;
            for (int e = 0; e < (k == 3 ? 2 : 16); ++e) {
                // NOLINTNEXTLINE(performance-inefficient-vector-operation): `together` is a fixture list built once; sizing it first would not change what the case checks
                together.push_back(FilterEntry{.eq = std::format("v{}", e)});
            }
            time("G1 any, oneOf of 2, " + name, {.group = name, .include = {FilterEntry{.oneOf = setOf(2, 2)}}});
            if (!unloaded) {
                time("G2 any, oneOf of 1,000, " + name,
                     {.group = name, .include = {FilterEntry{.oneOf = setOf(2, 1000)}}});
                time("G3 atLeast 2, oneOf of 2, " + name,
                     {.group = name,
                      .match = ColumnCount{.kind = ColumnCount::Kind::AtLeast, .n = 2},
                      .include = {FilterEntry{.oneOf = setOf(2, 2)}}});
                time("G3 exactly 1, oneOf of 2, " + name,
                     {.group = name,
                      .match = ColumnCount{.kind = ColumnCount::Kind::Exactly, .n = 1},
                      .include = {FilterEntry{.oneOf = setOf(2, 2)}}});
            }
            time(std::format("G4 together, {} entries, {}", together.size(), name),
                 {.group = name, .mode = GroupMode::Together, .include = together});
            if (!unloaded) {
                auto const first = engine.distinctValues({.group = name});
                REQUIRE(first);
                REQUIRE(first->values.size() == 32);
                printRow("G6 pooled distinct values, first 200, " + name, 100'000,
                         timedRuns(kRuns, [&] { std::ignore = (engine.distinctValues({.group = name})); }));
            }
        }
    }
}

// T1: a stored state for 300 columns with 8 sort keys and 32 filters, read, fitted and applied to a layout; no store
// is involved. Bound 2 ms. Mutation: fitState dropping every filter (the survival REQUIREs fail).
TEST_CASE("table state benchmark: parse, fit and apply 300 columns, 8 keys, 32 filters",
          "[.benchmark][table][state]") {
    constexpr std::size_t kColumns = 300;
    TableDescription description{.id = "wide", .label = "Wide", .columns = {}, .key = "c000"};
    std::vector<std::string> ids;
    for (std::size_t at = 0; at < kColumns; ++at) {
        auto id = std::format("c{:03}", at);
        // NOLINTNEXTLINE(readability-avoid-nested-conditional-operator): a three-way pick of the column kind by `at % 3`
        auto const kind = at % 3 == 0 ? ColumnKind::Integer : at % 3 == 1 ? ColumnKind::Text : ColumnKind::Decimal;
        description.columns.push_back(TableColumn{
            .info = {.id = id, .kind = kind, .comparator = {}, .format = {}, .level = {}}, .label = id, .group = {}});
        ids.push_back(std::move(id));
    }
    description = describe(std::move(description));
    StateDocument stored;
    ColumnLayoutValue layout;
    layout.order.assign(ids.rbegin(), ids.rend());
    for (std::size_t at = 0; at < kColumns; at += 10) {
        layout.hidden.push_back(ids[at]);
        layout.widths.emplace(ids[at], 12);
    }
    layout.pinned = 2;
    stored.layout = std::move(layout);
    SortChain sort;
    for (std::size_t at = 0; at < 8; ++at) {
        sort.push_back(SortKey{.column = ids[at * 7], .dir = at % 2 == 0 ? kAsc : kDesc});
    }
    stored.sort = std::move(sort);
    FilterSpec filters;
    for (std::size_t at = 0; at < 32; ++at) {
        auto const& id = ids[at * 9];
        FilterEntry entry = description.columns[at * 9].info.kind == ColumnKind::Text ? FilterEntry{.contains = "a"}
                                                                                      : FilterEntry{.gt = "5"};
        filters.columns.emplace(id,
                                ColumnFilter{.combine = Combine::Any, .include = {std::move(entry)}, .exclude = {}});
    }
    stored.filters = std::move(filters);
    std::string const json = writeState(stored);

    FitReport check;
    StateDocument const survived = fitState(readState(json).value(), description, check);
    REQUIRE(survived.sort->size() == 8);
    REQUIRE(survived.filters->columns.size() == 32);
    REQUIRE(check.dropped.empty());

    constexpr int kRuns = 31;
    std::vector<double> samples;
    for (int run = 0; run < kRuns; ++run) {
        auto const began = Clock::now();
        FitReport report;
        StateDocument const fitted = fitState(readState(json).value(), description, report);
        ColumnLayout applied{ids};
        applied.apply(*fitted.layout);
        samples.push_back(std::chrono::duration<double, std::milli>(Clock::now() - began).count());
        REQUIRE(applied.order().size() == kColumns);
    }
    printHeader("T1: parse, fit and apply a stored state (bound 2 ms)");
    printRow("300 columns, 8 keys, 32 filters", kColumns, spreadOf(samples));
}

// ── Values with units (M1 to M4) ────────────────────────────────────────────

TEST_CASE("table benchmark: values with units", "[.benchmark][table][measure]") {
    // 100,000 rows: a Measure column `m` over the fixture's registry (three dimensions, ten units), a Quantity column
    // `q` holding the same amounts with their units' factors, and a Decimal column `d` holding the amounts alone.
    constexpr std::size_t kRows = 100'000;
    constexpr int kRuns = 9;
    auto const& units = bench::wideUnits();
    // NOLINTNEXTLINE(bugprone-random-generator-seed,cert-msc32-c,cert-msc51-cpp) -- a fixed seed keeps the case reproducible
    std::mt19937 random{7};
    std::uniform_int_distribution<std::int64_t> amounts{0, 1'000'000};
    std::uniform_int_distribution<UnitId> unitOf{0, static_cast<UnitId>(units->size() - 1)};
    std::vector<std::vector<Cell>> rows;
    rows.reserve(kRows);
    for (std::size_t row = 0; row < kRows; ++row) {
        morph::math::Rational const amount{morph::math::Numerator{amounts(random)}, morph::math::Denominator{100},
                                           morph::math::DecimalPlaces{2}};
        auto const unit = unitOf(random);
        rows.push_back({Cell{Measure{.amount = amount, .unit = unit}},
                        Cell{QuantityCell{.amount = amount, .toCanonical = units->info(unit).toBase}}, Cell{amount}});
    }
    auto const source =
        tabletest::makeSource({tabletest::col("m", ColumnKind::Measure), tabletest::col("q", ColumnKind::Quantity),
                               tabletest::col("d", ColumnKind::Decimal)},
                              std::move(rows));
    ApplyOptions options;
    options.services.units = units;
    auto const sorted = [](std::string column) {
        TableQuery query;
        query.sort = {SortKey{.column = std::move(column), .dir = kAsc}};
        return query;
    };
    auto const greater = [](std::string column, std::string operand) {
        TableQuery query;
        query.filters.columns.emplace(
            std::move(column),
            ColumnFilter{.combine = Combine::Any, .include = {FilterEntry{.gt = std::move(operand)}}, .exclude = {}});
        return query;
    };
    auto const run = [&](TableQuery const& query) {
        auto const page = apply(*source, query, options);
        REQUIRE(page.has_value());
    };
    printHeader("M1-M4: values with units");
    printRow("M1: keys and sort, Measure m", kRows, timedRuns(kRuns, [&] { run(sorted("m")); }));
    printRow("M1: keys and sort, Quantity q", kRows, timedRuns(kRuns, [&] { run(sorted("q")); }));
    printRow("M2: gt \"250 g\", Measure m", kRows, timedRuns(kRuns, [&] { run(greater("m", "250 g")); }));
    printRow("M2: gt \"0.25\", Quantity q", kRows, timedRuns(kRuns, [&] { run(greater("q", "0.25")); }));

    std::array<std::string_view, 9> const ids{"kg", "g", "mg", "t", "l", "ml", "m3", "percent", "permille"};
    std::vector<std::string> operands;
    std::vector<std::string> typed;
    for (std::size_t at = 0; at < 1'000; ++at) {
        auto const amount = std::format("{}.{:02}", at * 7, at % 100);
        operands.push_back(amount + " " + std::string{ids.at(at % ids.size())});
        typed.push_back(std::format("{},{:02} {}", at * 7, at % 100, ids.at(at % ids.size())));
    }
    FilterSpec oneOf;
    oneOf.columns.emplace(
        "m", ColumnFilter{.combine = Combine::Any, .include = {FilterEntry{.oneOf = operands}}, .exclude = {}});
    auto const columns = source->columns();
    REQUIRE(compileFilter(oneOf, columns, options.services).has_value());
    printRow("M3: compile a oneOf of 1,000 operands with units", 1'000,
             timedRuns(31, [&] { REQUIRE(compileFilter(oneOf, columns, options.services).has_value()); }));
    std::vector<UnitInfo> listed;
    for (UnitId unit = 0; unit < units->size(); ++unit) {
        // NOLINTNEXTLINE(performance-inefficient-vector-operation): `listed` is a fixture list built once; sizing it first would not change what the case checks
        listed.push_back(units->info(unit));
    }
    auto const& de = morph::ui::localeFacts("de");
    REQUIRE(morph::ui::parseMeasure(typed.front(), listed, de).has_value());
    std::size_t parsed = 0;
    printRow("M3: ui::parseMeasure of 1,000 typed texts", 1'000, timedRuns(31, [&] {
                 for (auto const& text : typed) {
                     parsed += morph::ui::parseMeasure(text, listed, de).has_value() ? 1U : 0U;
                 }
             }));
    REQUIRE(parsed == 31 * typed.size());

    auto const values = [&](std::string column) {
        auto const listedValues =
            distinctValues(*source, TableQuery{}, DistinctRequest{.column = std::move(column)}, options);
        REQUIRE(listedValues.has_value());
        REQUIRE(listedValues->values.size() == 200);
    };
    printRow("M4: distinctValues, Measure m, first 200", kRows, timedRuns(kRuns, [&] { values("m"); }));
    printRow("M4: distinctValues, Decimal d, first 200", kRows, timedRuns(kRuns, [&] { values("d"); }));
    // As D1 is measured: through an engine that holds the column's keys, so only the grouping is timed.
    auto const held = [&](std::string column) {
        EngineOptions engineOptions;
        engineOptions.services.units = units;
        Engine engine{source, engineOptions};
        require(engine.setSort({SortKey{.column = column, .dir = kAsc}}));
        REQUIRE(engine.stats().keyBuilds == 1);
        return timedRuns(kRuns, [&] {
            auto const listedValuesHeld = engine.distinctValues({.column = column});
            REQUIRE(listedValuesHeld.has_value());
            REQUIRE(listedValuesHeld->values.size() == 200);
        });
    };
    printRow("M4: distinctValues through an engine holding the keys, Measure m", kRows, held("m"));
    printRow("M4: distinctValues through an engine holding the keys, Decimal d", kRows, held("d"));
}

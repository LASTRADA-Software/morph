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
#include <exception>
#include <format>
#include <functional>
#include <glaze/glaze.hpp>
#include <memory>
#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/wire.hpp>
#include <morph/detail/reflected_member.hpp>
#include <morph/session/session.hpp>
#include <morph/table/description.hpp>
#include <morph/table/engine.hpp>
#include <morph/table/query.hpp>
#include <optional>
#include <print>
#include <random>
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
        auto const* const quantity = std::get_if<QuantityCell>(&cell);
        member = quantity != nullptr ? quantity->amount : std::get<morph::math::Rational>(cell);
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
    auto const page40 = writeGenericPage(rows, wide, forty);
    REQUIRE(cellsOf(page40) == 8'000);
    auto const body40 = jsonOf(page40);
    auto const envelope40 = morph::wire::encode(morph::wire::makeOk(1, body40)).size();
    printWire("W1", "200 x 40 generic page in an ok envelope", envelope40, Spread{});
    printWire("W2", "encode 200 x 40 from the column store", body40.size(),
              timed(kRuns, [&] { REQUIRE(jsonOf(writeGenericPage(rows, wide, forty)).size() == body40.size()); }));
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
    auto const page30 = writeGenericPage(rows, small, thirty);
    REQUIRE(cellsOf(page30) == 6'000);
    auto const generic30 = jsonOf(page30).size();
    auto const typed = typedPage30(small, rows);
    REQUIRE(typed.rows.size() == 200);
    auto const typed30 = jsonOf(typed).size();
    printWire("W5", "200 x 30 generic page, encode", generic30,
              timed(kRuns, [&] { REQUIRE(jsonOf(writeGenericPage(rows, small, thirty)).size() == generic30); }));
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
    auto const page300 = writeGenericPage(rows, wide, all);
    REQUIRE(cellsOf(page300) == 60'000);
    auto const body300 = jsonOf(page300);
    auto const envelope300 = morph::wire::encode(morph::wire::makeOk(1, body300)).size();
    REQUIRE(envelope300 < morph::wire::kMaxEnvelopeBytes);
    printWire("W7", "200 x 300 generic page in an ok envelope", envelope300, Spread{});
    printWire("W7", "encode 200 x 300 from the column store", body300.size(),
              timed(15, [&] { REQUIRE(jsonOf(writeGenericPage(rows, wide, all)).size() == body300.size()); }));
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
        count += std::holds_alternative<Unloaded>(snapshot->cell(row, *index)) ? 1 : 0;
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
        auto const page = apply(*partial, s1);
        REQUIRE(page.has_value());
        REQUIRE(page->rows.size() == 200);
        auto const filtered = apply(*partial, s2);
        REQUIRE(filtered.has_value());
        REQUIRE(filtered->undecided > 25'000);
        REQUIRE(filtered->totalKind == TotalKind::AtLeast);
    }
    printHeader("Step 2a: apply over the wide table with the recipe level, 100,000 x 300, in the calling thread");
    auto const time = [&](std::string const& scenario, DataSource const& source, TableQuery const& query) {
        std::vector<double> samples;
        for (int run = 0; run < runsFor(kWideRows); ++run) {
            auto const began = Clock::now();
            auto const page = apply(source, query);
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
        Engine engine{source};
        require(engine.setSort({{.column = "d000", .dir = kAsc}}));
        auto const waiting = source->unloadedRows();
        REQUIRE(waiting.size() > 25'000);
        for (auto const [from, count, samples] : {std::tuple{std::size_t{0}, std::size_t{200}, &first},
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
        Engine engine{source};
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

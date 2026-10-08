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
#include <memory>
#include <morph/core/executor.hpp>
#include <morph/core/wire.hpp>
#include <morph/table/engine.hpp>
#include <morph/table/query.hpp>
#include <print>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "table/table_fixtures.hpp"

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

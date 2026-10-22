// SPDX-License-Identifier: Apache-2.0
//
// Mount-layer measurements for the table row virtualization design (docs/superpowers/specs,
// "Table row virtualization — design", Task 0): hidden, so ctest never runs it; run it by tag on
// a Release build:
//
//   morph_tests "[.benchmark][mount]"
//
// Two questions, both load-bearing for that design's Option A (a row's reactive state stays alive
// for every key in the data; only its widget comes and goes with the viewport):
//
// 1. Re-measure the original #1016 repro (a table scaling past ~2,000 rows exhausted memory and
//    never finished mounting) against current master, which has since merged the #1007 fix (an
//    O(rows x columns) column-width rescan in the same code path). If that fix already brought the
//    cost back to roughly linear, the remaining problem is purely the realized-widget count at very
//    large row counts, not a second compounding defect.
// 2. Whether a row's cost scales mostly with its cell count (the realized widget tree the eager
//    mount builds for every row today) or is already dominated by fixed per-row overhead regardless
//    of content, which is the closest approximation this codebase can make today to "scope-only
//    mounting cost" -- there is no widget-less mount path to measure directly before Task 1 builds
//    one, so this compares a minimal-cell row against a multi-cell row at the same row counts as a
//    proxy, not a direct measurement of Option A's premise.
//
// Uses RecordingBackend (no real Qt Quick widgets), so these numbers isolate the view-tree mount's
// own cost from any toolkit rendering cost -- a lower bound on what a real backend will cost, not
// an upper one.

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/signal.hpp>
#include <morph/testing/step_executor.hpp>
#include <morph/ui/mount.hpp>
#include <morph/ui/testing/recording_backend.hpp>
#include <morph/ui/view.hpp>
#include <print>
#include <string>
#include <vector>

namespace ui = morph::ui;

namespace {

using Clock = std::chrono::steady_clock;
using morph::reactive::Runtime;
using morph::reactive::Signal;
using Owner = morph::testing::StepExecutor;
using ui::testing::RecordingBackend;

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
    if (rows >= 5000) {
        return 3;
    }
    return rows >= 1000 ? 5 : 9;
}

void printHeader(char const* title) {
    std::println("\n{}\n\n| scenario | rows | median ms | p10-p90 ms | live nodes |\n|---|---:|---:|---:|---:|",
                 title);
}

void printRow(std::string const& scenario, std::size_t rows, Spread const& spread, std::size_t liveNodes) {
    std::println("| {} | {} | {:.2f} | {:.2f}-{:.2f} | {} |", scenario, rows, spread.median, spread.p10, spread.p90,
                 liveNodes);
    static_cast<void>(std::fflush(stdout));
}

struct Row {
    std::int64_t id = 0;
    std::string name;
    std::string description;
    bool picked = false;
};

std::vector<Row> rowsOf(std::size_t count) {
    std::vector<Row> rows;
    rows.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        rows.push_back(Row{.id = static_cast<std::int64_t>(i),
                           .name = "row-" + std::to_string(i),
                           .description = "a description long enough to be realistic for row " + std::to_string(i),
                           .picked = i % 2 == 0});
    }
    return rows;
}

// Mirrors #1007's own repro shape: one checkbox cell plus two text cells per row -- the realized
// widget tree an eager mount builds today for every row, regardless of whether it is visible.
ui::Node multiCellTable(std::vector<Row> const& data) {
    return ui::table<Row>(
        {{.label = "Picked"}, {.label = "Name"}, {.label = "Description"}}, [data] { return data; },
        [](Row const& row) { return ui::Key{row.id}; },
        [](Signal<Row> const& row) {
            return std::vector<ui::Node>{ui::checkbox({.checked = [&row] { return row.get().picked; }}),
                                         ui::text({.text = [&row] { return row.get().name; }}),
                                         ui::text({.text = [&row] { return row.get().description; }})};
        });
}

// The minimal row shape: one text cell, nothing else -- the closest proxy available today to "just
// the row's state, no meaningful widget tree," since there is no widget-less mount path to measure
// directly.
ui::Node minimalCellTable(std::vector<Row> const& data) {
    return ui::table<Row>(
        {{.label = "Name"}}, [data] { return data; }, [](Row const& row) { return ui::Key{row.id}; },
        [](Signal<Row> const& row) {
            return std::vector<ui::Node>{ui::text({.text = [&row] { return row.get().name; }})};
        });
}

void measureMount(std::string const& scenario, std::size_t rows,
                  std::function<ui::Node(std::vector<Row> const&)> const& build) {
    std::vector<Row> const data = rowsOf(rows);
    std::vector<double> samples;
    std::size_t liveNodes = 0;
    for (int run = 0; run < runsFor(rows); ++run) {
        Owner owner;
        Runtime runtime{owner};
        RecordingBackend backend;
        auto const began = Clock::now();
        {
            ui::Mounted const mounted{runtime, backend, build(data)};
            samples.push_back(std::chrono::duration<double, std::milli>(Clock::now() - began).count());
            liveNodes = runtime.core()->liveNodes();
        }
    }
    printRow(scenario, rows, spreadOf(samples), liveNodes);
}

constexpr std::array<std::size_t, 6> kSizes{500, 1000, 2000, 5000, 10000, 20000};

}  // namespace

TEST_CASE("mount benchmark: #1016's repro shape, re-measured on current master", "[.benchmark][mount]") {
    printHeader("Mounting a multi-cell table (checkbox + 2 text columns per row, #1007/#1016's own repro shape)");
    for (std::size_t const rows : kSizes) {
        measureMount("multi-cell (checkbox + 2 text)", rows, multiCellTable);
    }
}

TEST_CASE("mount benchmark: a minimal single-cell row, as a proxy for state-only mounting cost",
          "[.benchmark][mount]") {
    printHeader(
        "Mounting a minimal table (one text cell per row) -- the closest available proxy for "
        "scope-only cost without a widget-less mount path");
    for (std::size_t const rows : kSizes) {
        measureMount("minimal (1 text cell)", rows, minimalCellTable);
    }
}

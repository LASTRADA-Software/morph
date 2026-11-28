// SPDX-License-Identifier: Apache-2.0
//
// The frontend baseline of docs/spec/table/table.md ("The frontend at 300 columns"): a `rows` table of the wide-table
// fixture's 2,000 x 300 cells in a 1,280 x 800 offscreen window, before any column window exists. Hidden; run it by
// tag on a Release build with QT_QPA_PLATFORM=offscreen:
//
//   morph_qt_quick_tests "[.benchmark][baseline]"
//
// The windowed case measures the same figures on a table over a `TableSource` with the column window:
//
//   morph_qt_quick_tests "[.benchmark][windowed]"
//
// A frame is timed as the spec's CI method says: the test moves the scroll position one step, then times the work
// that step causes on the software scene graph -- the owner's realization, Qt's events, and polish, sync and render
// through QQuickWindow::grabWindow. It is CPU time for one frame's work, comparable between commits on one machine.

#include <QCoreApplication>
#include <QImage>
#include <QQuickItem>
#include <QQuickWindow>
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <morph/reactive/testing/manual_scheduler.hpp>
#include <morph/ui/interpreter.hpp>
#include <morph/ui/mount.hpp>
#include <morph/ui/view.hpp>
#include <mutex>
#include <print>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "qt_quick/widget.hpp"
#include "qt_quick_harness.hpp"
#include "table/wide_table_model.hpp"
#include "ui_fake_rows.hpp"
#include "ui_interpreter_support.hpp"
#include "wide_rows_document.hpp"

namespace ui = morph::ui;
namespace detail = morph::qt_quick::detail;
using morph::qt_quick::testing::Scene;

namespace {

using Clock = std::chrono::steady_clock;

struct Percentiles {
    double p10 = 0;
    double p50 = 0;
    double p90 = 0;
    double p99 = 0;
};

Percentiles percentilesOf(std::vector<double> samples) {
    std::ranges::sort(samples);
    auto const at = [&](double q) {
        auto const index = static_cast<std::size_t>(std::lround(q * static_cast<double>(samples.size() - 1)));
        return samples[std::min(index, samples.size() - 1)];
    };
    return Percentiles{.p10 = at(0.1), .p50 = at(0.5), .p90 = at(0.9), .p99 = at(0.99)};
}

// Bounds every step of the baseline: a step that runs past its budget is named on stderr and ends the process with
// exit code 3, so a step that blocks is a reported failure and not a run that never ends.
class Watchdog {
public:
    Watchdog() = default;
    Watchdog(Watchdog const&) = delete;
    Watchdog& operator=(Watchdog const&) = delete;
    Watchdog(Watchdog&&) = delete;
    Watchdog& operator=(Watchdog&&) = delete;
    ~Watchdog() = default;

    // Starts @p name, which must end within @p budget.
    void step(std::string name, std::chrono::seconds budget) {
        std::scoped_lock const lock{_mutex};
        auto const now = Clock::now();
        if (!_name.empty()) {
            std::println(stderr, "qt quick baseline: '{}' took {:.1f} s", _name,
                         std::chrono::duration<double>(now - _began).count());
        }
        std::println(stderr, "qt quick baseline: {} (budget {} s)", name, budget.count());
        static_cast<void>(std::fflush(stderr));
        _name = std::move(name);
        _budget = budget;
        _began = now;
        _deadline = now + budget;
    }

private:
    void watch(std::stop_token const& stop) {
        while (!stop.stop_requested()) {
            std::this_thread::sleep_for(std::chrono::milliseconds{200});
            std::scoped_lock const lock{_mutex};
            if (Clock::now() > _deadline) {
                std::println(stderr, "qt quick baseline: FAILED: '{}' ran past its {} s budget", _name,
                             _budget.count());
                static_cast<void>(std::fflush(stderr));
                static_cast<void>(std::fflush(stdout));
                std::_Exit(3);
            }
        }
    }

    std::mutex _mutex;
    std::string _name;
    std::chrono::seconds _budget{0};
    Clock::time_point _began;
    Clock::time_point _deadline = Clock::time_point::max();
    std::jthread _thread{[this](std::stop_token const& stop) { watch(stop); }};  // last: starts once the rest exists
};

// One client on the Qt Quick backend in a 1,280 x 800 window, with no model behind it: the table's rows are literal.
struct BenchHarness {
    Scene scene;
    morph::reactive::testing::ManualScheduler scheduler;
    ui::Session session{scene.runtime(), ui::SessionOptions{.scheduler = &scheduler,
                                                            .clock = [this] { return scheduler.now(); },
                                                            .keepUnused = std::chrono::minutes{5},
                                                            .catalog = {},
                                                            .classify = {}}};
    ui::testing::ScriptedDispatcher dispatcher{scene.owner()};
    ui::Interpreter interpreter{session, dispatcher, ui::InterpreterOptions{}};

    BenchHarness() {
        scene.window().resize(1280, 800);
        scene.settle();
    }

    // Runs what one step of scrolling causes, and renders the frame; returns its time in milliseconds.
    double frame() {
        auto const began = Clock::now();
        scene.owner().runAll();
        QCoreApplication::processEvents(QEventLoop::AllEvents);
        scene.owner().runAll();
        QCoreApplication::processEvents(QEventLoop::AllEvents);
        QImage const image = scene.window().grabWindow();
        auto const millis = std::chrono::duration<double, std::milli>(Clock::now() - began).count();
        REQUIRE_FALSE(image.isNull());
        return millis;
    }
};

// The table reports its visible range to C++ only when it scrolls, so after a mount or a resize it holds the rows
// it had; a scroll of one pixel and back realizes what the viewport shows, which is what F2 to F5 measure.
void reportRange(BenchHarness& harness, QQuickItem& flick) {
    double const at = flick.property("contentY").toReal();
    flick.setProperty("contentY", at + 1.0);
    static_cast<void>(harness.frame());
    flick.setProperty("contentY", at);
    static_cast<void>(harness.frame());
    harness.scene.settle();
}

// Scrolls @p flick's @p property from its current value by @p step a frame for @p frames frames, timing each.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the step per frame, then the limit, as the cases state them.
std::vector<double> scrollFrames(BenchHarness& harness, QQuickItem& flick, char const* property, double step,
                                 int frames, double limit) {
    std::vector<double> times;
    times.reserve(static_cast<std::size_t>(frames));
    double position = flick.property(property).toReal();
    for (int index = 0; index < frames; ++index) {
        position = std::min(limit, position + step);
        flick.setProperty(property, position);
        times.push_back(harness.frame());
    }
    return times;
}

void printFrames(std::string_view scenario, std::vector<double> const& times) {
    auto const p = percentilesOf(times);
    auto const over = std::ranges::count_if(times, [](double t) { return t > 16.6; });
    std::println("| {} | {} | {:.2f} | {:.2f}-{:.2f} | {:.2f} | {} |", scenario, times.size(), p.p50, p.p10, p.p90,
                 p.p99, over);
    static_cast<void>(std::fflush(stdout));
}

}  // namespace

TEST_CASE("qt quick baseline: F2 to F5 on a rows table of 2,000 x 300", "[.benchmark][baseline]") {
    using std::chrono::seconds;
    Watchdog watchdog;
    watchdog.step("mount: the document's text", seconds{120});
    bench::WideTableSource const source{{.rows = 2'000, .columns = 300, .seed = 7}};
    std::string const document = bench::wideRowsDocument(source);
    BenchHarness harness;
    auto const memoryBefore = bench::processBytes();
    auto const mountBegan = Clock::now();
    watchdog.step("mount: parse", seconds{300});
    auto const loaded = ui::testing::loadOrFail(document);
    watchdog.step("mount: interpreter mount", seconds{300});
    auto const screen = harness.interpreter.mount(loaded, {}, harness.scene.backend());
    REQUIRE(screen.has_value());
    watchdog.step("mount: settle", seconds{300});
    harness.scene.settle();
    watchdog.step("mount: first frame", seconds{300});
    static_cast<void>(harness.frame());
    auto const mountMillis = std::chrono::duration<double, std::milli>(Clock::now() - mountBegan).count();

    detail::ItemBase const* const table = detail::findWidget(QStringLiteral("wide"));
    REQUIRE(table != nullptr);
    QQuickItem const* const tableItem = table->item();
    REQUIRE(tableItem != nullptr);
    auto* const flick = tableItem->findChild<QQuickItem*>(QStringLiteral("flick"));
    auto* const hflick = tableItem->findChild<QQuickItem*>(QStringLiteral("hflick"));
    REQUIRE(flick != nullptr);
    REQUIRE(hflick != nullptr);
    REQUIRE(tableItem->property("totalRows").toInt() == 2'000);
    int const realizedAtMount = tableItem->property("realizedCount").toInt();
    watchdog.step("first range report", seconds{120});
    reportRange(harness, *flick);
    int const realized = tableItem->property("realizedCount").toInt();
    REQUIRE(realized >= 30);

    // F2: widgets alive, and header cells.
    auto const widgets = detail::liveWidgets().size();
    std::size_t headers = 0;
    for (QQuickItem const* const item : tableItem->findChildren<QQuickItem*>()) {
        headers += item->objectName().startsWith(QStringLiteral("header")) ? 1U : 0U;
    }
    std::println(
        "\nF2: a rows table of 2,000 x 300, 1,280 x 800, mounted in {:.0f} ms (document parse included), {} rows "
        "realized at the mount\n\n"
        "| realized rows | widgets alive | header cells | widgets per realized row |\n|---:|---:|---:|---:|\n"
        "| {} | {} | {} | {:.1f} |",
        mountMillis, realizedAtMount, realized, widgets, headers,
        static_cast<double>(widgets) / static_cast<double>(std::max(realized, 1)));
    static_cast<void>(std::fflush(stdout));

    // F3 and F4: frame times of vertical and horizontal scrolls.
    double const rowHeight = flick->property("contentHeight").toReal() / 2'000.0;
    double const columnWidth = hflick->property("contentWidth").toReal() / 300.0;
    double const bottom = std::max(0.0, flick->property("contentHeight").toReal() - flick->height());
    double const right = std::max(0.0, hflick->property("contentWidth").toReal() - hflick->width());
    REQUIRE(rowHeight > 0);
    REQUIRE(columnWidth > 0);
    REQUIRE(right > 0);
    std::println(
        "\nF3, F4: frame time, CPU, software scene graph, offscreen (row {:.1f} px, column {:.1f} px)\n\n"
        "| scroll | frames | p50 ms | p10-p90 ms | p99 ms | frames over 16.6 ms |\n|---|---:|---:|---:|---:|---:|",
        rowHeight, columnWidth);
    static_cast<void>(std::fflush(stdout));
    constexpr int kFrames = 120;  // two seconds at 60 frames a second
    struct Scroll {
        char const* name;
        QQuickItem* flickable;
        char const* property;
        double step;
        double limit;
    };
    std::vector<Scroll> const scrolls{
        {.name = "F3: vertical, 20 rows/s",
         .flickable = flick,
         .property = "contentY",
         .step = rowHeight * 20.0 / 60.0,
         .limit = bottom},
        {.name = "F3: vertical fling, 2,000 rows/s",
         .flickable = flick,
         .property = "contentY",
         .step = rowHeight * 2'000.0 / 60.0,
         .limit = bottom},
        {.name = "F3: thumb drag across the table in 2 s",
         .flickable = flick,
         .property = "contentY",
         .step = bottom / kFrames,
         .limit = bottom},
        {.name = "F4: horizontal, 10 columns/s",
         .flickable = hflick,
         .property = "contentX",
         .step = columnWidth * 10.0 / 60.0,
         .limit = right},
        {.name = "F4: horizontal fling, 100 columns/s",
         .flickable = hflick,
         .property = "contentX",
         .step = columnWidth * 100.0 / 60.0,
         .limit = right},
    };
    for (Scroll const& scroll : scrolls) {
        watchdog.step(scroll.name, seconds{120});
        printFrames(scroll.name,
                    scrollFrames(harness, *scroll.flickable, scroll.property, scroll.step, kFrames, scroll.limit));
        scroll.flickable->setProperty(scroll.property, 0.0);
        static_cast<void>(harness.frame());
    }

    // F5: memory per realized row, from the growth when a taller window realizes more rows.
    watchdog.step("F5: resize to 1,600 px and report the range", seconds{120});
    auto const memoryMounted = bench::processBytes();
    harness.scene.window().resize(1280, 1600);
    harness.scene.settle();
    reportRange(harness, *flick);
    int const realizedTaller = tableItem->property("realizedCount").toInt();
    auto const memoryTaller = bench::processBytes();
    REQUIRE(realizedTaller > realized);
    double const perRow = (static_cast<double>(memoryTaller) - static_cast<double>(memoryMounted)) /
                          static_cast<double>(realizedTaller - realized);
    std::println(
        "\nF5: memory per realized row, from a window of 800 px to one of 1,600 px\n\n"
        "| realized rows | process bytes after mount (over before) | rows added | bytes added | bytes per row |\n"
        "|---|---:|---:|---:|---:|\n| {} -> {} | {} | {} | {} | {:.0f} |",
        realized, realizedTaller, static_cast<std::int64_t>(memoryMounted) - static_cast<std::int64_t>(memoryBefore),
        realizedTaller - realized, static_cast<std::int64_t>(memoryTaller) - static_cast<std::int64_t>(memoryMounted),
        perRow);
    static_cast<void>(std::fflush(stdout));
    watchdog.step("teardown", seconds{300});
}

namespace {

std::string benchField(ui::Value const& record, std::string const& field) {
    ui::Value const* const value = record.field(field);
    return value != nullptr && value->asString() != nullptr ? *value->asString() : std::string{};
}

}  // namespace

// The same figures with the column window: a table over a `TableSource` of 100,000 x 300, each column 8 units, in the
// same window. It first asserts the table is windowed both ways, so a mount that realized every column could not pass
// for the windowed one.
TEST_CASE("qt quick windowed: F3 to F5 on a source table of 100,000 x 300", "[.benchmark][windowed]") {
    using std::chrono::seconds;
    Watchdog watchdog;
    watchdog.step("mount", seconds{300});
    BenchHarness harness;
    auto const rows = std::make_shared<morph::testing::FakeRows>(100'000, 300, harness.scene.owner());
    std::vector<ui::TableColumn> columns;
    columns.reserve(300);
    for (std::size_t column = 0; column < 300; ++column) {
        columns.push_back(ui::TableColumn{
            .id = "c" + std::to_string(column), .label = "C" + std::to_string(column), .width = ui::Sizing::fixed(8)});
    }
    ui::TableSource source;
    source.rows = rows;
    source.ownCell.assign(300, false);
    source.defaultCellView = [](morph::reactive::Signal<std::size_t> const& column,
                                morph::reactive::Signal<ui::Value> const& row) {
        return ui::text(
            {.text = [&column, &row] { return benchField(row.get(), "c" + std::to_string(column.get())); }});
    };
    source.cellView = [](std::size_t column, morph::reactive::Signal<ui::Value> const& row) {
        return ui::text({.text = [&row, column] { return benchField(row.get(), "c" + std::to_string(column)); }});
    };
    auto const memoryBefore = bench::processBytes();
    ui::Mounted const view{harness.scene.runtime(), harness.scene.backend(),
                           ui::table(std::move(columns), std::move(source), ui::TableOptions{})};
    harness.scene.settle();
    static_cast<void>(harness.frame());
    QQuickItem const* const tableItem = &Scene::itemOf(view.root());
    auto* const flick = tableItem->findChild<QQuickItem*>(QStringLiteral("flick"));
    auto* const hflick = tableItem->findChild<QQuickItem*>(QStringLiteral("hflick"));
    REQUIRE(flick != nullptr);
    REQUIRE(hflick != nullptr);
    reportRange(harness, *flick);

    // Windowed both ways.
    int const realized = tableItem->property("realizedCount").toInt();
    int const windowColumns = tableItem->property("realizedColumnCount").toInt();
    REQUIRE(tableItem->property("arranged").toBool());
    REQUIRE(realized > 0);
    REQUIRE(realized < 100);
    REQUIRE(windowColumns > 0);
    REQUIRE(windowColumns < 100);
    std::println("\nwindowed: {} rows x {} columns realized, {} header cells, {} cells created", realized,
                 windowColumns, tableItem->property("headerCellCount").toInt(), view.stats().cellsCreated);

    double const rowHeight = tableItem->property("meanRowHeight").toReal();
    double const columnWidth = tableItem->property("windowTotalWidth").toReal() / 300.0;
    double const bottom = std::max(0.0, flick->property("contentHeight").toReal() - flick->height());
    double const right = std::max(0.0, hflick->property("contentWidth").toReal() - hflick->width());
    REQUIRE(rowHeight > 0);
    REQUIRE(columnWidth > 0);
    REQUIRE(right > 0);
    std::println(
        "\nF3, F4 windowed: frame time, CPU, software scene graph, offscreen (row {:.1f} px, column {:.1f} px)"
        "\n\n| scroll | frames | p50 ms | p10-p90 ms | p99 ms | frames over 16.6 ms |\n"
        "|---|---:|---:|---:|---:|---:|",
        rowHeight, columnWidth);
    static_cast<void>(std::fflush(stdout));
    constexpr int kFrames = 120;
    struct Scroll {
        char const* name;
        QQuickItem* flickable;
        char const* property;
        double step;
        double limit;
    };
    std::vector<Scroll> const scrolls{
        {.name = "F3: vertical, 20 rows/s",
         .flickable = flick,
         .property = "contentY",
         .step = rowHeight * 20.0 / 60.0,
         .limit = bottom},
        {.name = "F3: vertical fling, 2,000 rows/s",
         .flickable = flick,
         .property = "contentY",
         .step = rowHeight * 2'000.0 / 60.0,
         .limit = bottom},
        {.name = "F3: thumb drag across the table in 2 s",
         .flickable = flick,
         .property = "contentY",
         .step = bottom / kFrames,
         .limit = bottom},
        {.name = "F4: horizontal, 10 columns/s",
         .flickable = hflick,
         .property = "contentX",
         .step = columnWidth * 10.0 / 60.0,
         .limit = right},
        {.name = "F4: horizontal fling, 100 columns/s",
         .flickable = hflick,
         .property = "contentX",
         .step = columnWidth * 100.0 / 60.0,
         .limit = right},
    };
    for (Scroll const& scroll : scrolls) {
        watchdog.step(scroll.name, seconds{120});
        printFrames(scroll.name,
                    scrollFrames(harness, *scroll.flickable, scroll.property, scroll.step, kFrames, scroll.limit));
        scroll.flickable->setProperty(scroll.property, 0.0);
        static_cast<void>(harness.frame());
        harness.scene.settle();
    }

    // F5: memory per realized row, from the growth when a taller window realizes more rows.
    watchdog.step("F5: resize to 1,600 px and report the range", seconds{120});
    auto const memoryMounted = bench::processBytes();
    harness.scene.window().resize(1280, 1600);
    harness.scene.settle();
    reportRange(harness, *flick);
    int const realizedTaller = tableItem->property("realizedCount").toInt();
    auto const memoryTaller = bench::processBytes();
    REQUIRE(realizedTaller > realized);
    double const perRow = (static_cast<double>(memoryTaller) - static_cast<double>(memoryMounted)) /
                          static_cast<double>(realizedTaller - realized);
    std::println(
        "\nF5 windowed: memory per realized row, from a window of 800 px to one of 1,600 px\n\n"
        "| realized rows | process bytes after mount (over before) | rows added | bytes added | bytes per row |\n"
        "|---|---:|---:|---:|---:|\n| {} -> {} | {} | {} | {} | {:.0f} |",
        realized, realizedTaller, static_cast<std::int64_t>(memoryMounted) - static_cast<std::int64_t>(memoryBefore),
        realizedTaller - realized, static_cast<std::int64_t>(memoryTaller) - static_cast<std::int64_t>(memoryMounted),
        perRow);
    static_cast<void>(std::fflush(stdout));
    watchdog.step("teardown", seconds{300});
}

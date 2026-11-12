// SPDX-License-Identifier: Apache-2.0
//
// The chart on Qt Quick: what its Canvas paints, read back from the window at the device pixel ratio the platform
// gives (the suite also runs it with QT_SCALE_FACTOR=2), and what it tells assistive technology.

#include <QAccessible>
#include <QColor>
#include <QImage>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <morph/ui/view.hpp>
#include <optional>
#include <utility>
#include <vector>

#include "qt_quick_harness.hpp"

namespace ui = morph::ui;
using morph::qt_quick::testing::Scene;

namespace {

// The theme's default error colour: what an `err` line and an `err` mark are drawn in.
constexpr QRgb kErr = 0xc62828;

bool isErr(QColor const& colour) {
    return std::abs(colour.red() - qRed(kErr)) <= 8 && std::abs(colour.green() - qGreen(kErr)) <= 8 &&
           std::abs(colour.blue() - qBlue(kErr)) <= 8;
}

// One dashed `err` line at the middle of 0 to 10, and three points, the last marked `err` far above the line.
ui::ChartData sample() {
    ui::ChartData data;
    data.xMin = -0.1;
    data.xMax = 2.1;
    data.yMin = 0.0;
    data.yMax = 10.0;
    data.lines = {{.value = 5.0, .label = "", .tone = ui::Tone::Err, .style = ui::ChartLineStyle::Dashed}};
    data.series = {
        {.label = {},
         .joined = false,
         .points = {{.x = 0.0, .y = 2.0}, {.x = 1.0, .y = 3.0}, {.x = 2.0, .y = 9.0, .tone = ui::Tone::Err}}}};
    data.summary = "Three runs, the last one high";
    return data;
}

// How many pixels of each row are the error colour.
std::vector<int> errPerRow(QImage const& image) {
    std::vector<int> rows(static_cast<std::size_t>(image.height()), 0);
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (isErr(image.pixelColor(x, y))) {
                ++rows.at(static_cast<std::size_t>(y));
            }
        }
    }
    return rows;
}

}  // namespace

// Mutation: skip drawing the reference lines in MorphChart.qml (no row then holds the dashed line).
TEST_CASE("qt_quick chart: a dashed err line, and a marked point as an err cross away from it", "[qt_quick][chart]") {
    Scene scene;
    scene.window().resize(400, 200);
    auto chart = scene.backend().createChart(nullptr);
    chart->setData(sample());
    scene.settle();
    QImage const image = scene.window().grabWindow();
    REQUIRE(image.height() >= 200);
    std::vector<int> const rows = errPerRow(image);
    // The line's row: most of the plot's width, broken by its gaps.
    auto const line = std::ranges::max_element(rows);
    int const lineRow = static_cast<int>(line - rows.begin());
    CHECK(*line > image.width() / 3);
    int gaps = 0;
    for (int x = 1; x < image.width(); ++x) {
        gaps += isErr(image.pixelColor(x - 1, lineRow)) && !isErr(image.pixelColor(x, lineRow)) ? 1 : 0;
    }
    CHECK(gaps > 10);
    // The cross: error pixels well above the line, where nothing else is the error colour.
    int above = 0;
    for (int y = 0; y < lineRow - (image.height() / 5); ++y) {
        above += rows.at(static_cast<std::size_t>(y));
    }
    CHECK(above > 4);
}

namespace {

// The theme's first series colour, which a series' bars are filled with.
constexpr QRgb kSeries1 = 0x0072b2;

bool isSeries1(QColor const& colour) {
    return std::abs(colour.red() - qRed(kSeries1)) <= 8 && std::abs(colour.green() - qGreen(kSeries1)) <= 8 &&
           std::abs(colour.blue() - qBlue(kSeries1)) <= 8;
}

// The runs of columns that hold the first series' colour, each as its first column and width, and the pixels the
// colour fills in the run's middle column.
struct Run {
    int first = 0;
    int width = 0;
    int filled = 0;
};

std::vector<Run> runsOf(QImage const& image) {
    std::vector<Run> runs;
    std::optional<int> start;
    auto const filledIn = [&image](int x) {
        int filled = 0;
        for (int y = 0; y < image.height(); ++y) {
            filled += isSeries1(image.pixelColor(x, y)) ? 1 : 0;
        }
        return filled;
    };
    for (int x = 0; x <= image.width(); ++x) {
        // A column belongs to a run when the colour fills more than 10 px of it, which a 5 px marker never does.
        bool const in = x < image.width() && filledIn(x) > 10;
        if (in && !start) {
            start = x;
        } else if (!in && start) {
            int const width = x - *start;
            runs.push_back({*start, width, filledIn(*start + (width / 2))});
            start.reset();
        }
    }
    return runs;
}

}  // namespace

// Mutation: draw a `Bars` series as points (no column holds more than 10 px of the series colour), leave the
// baseline out of the rectangle (the bars are as tall as their tips are high), or ignore `barSlot` (the bars are not
// four fifths of the distance between positions).
TEST_CASE("qt_quick chart: a bars series is filled rectangles from the baseline, four fifths of a slot wide",
          "[qt_quick][chart]") {
    Scene scene;
    scene.window().resize(400, 200);
    auto chart = scene.backend().createChart(nullptr);
    ui::ChartData data;
    data.xMin = -0.5;
    data.xMax = 1.5;
    data.yMin = 0.0;
    data.yMax = 8.0;
    data.barSlot = 1.0;
    data.series = {{.label = {},
                    .joined = false,
                    .style = ui::ChartStyle::Bars,
                    .points = {{.x = 0.0, .y = 8.0}, {.x = 1.0, .y = 2.0}}}};
    data.summary = "Two bars";
    chart->setData(data);
    scene.settle();
    QImage const image = scene.window().grabWindow();
    std::vector<Run> const runs = runsOf(image);
    REQUIRE(runs.size() == 2);
    // The positions are one slot apart, and a bar is 0.8 of it wide.
    double const slot = runs.at(1).first + (runs.at(1).width / 2.0) - (runs.at(0).first + (runs.at(0).width / 2.0));
    CHECK(runs.at(0).width == Catch::Approx(0.8 * slot).margin(2.0));
    CHECK(runs.at(1).width == Catch::Approx(0.8 * slot).margin(2.0));
    // The first bar reaches the axis's top and the second a quarter of the way up from the baseline.
    CHECK(runs.at(1).filled == Catch::Approx(runs.at(0).filled / 4.0).margin(3.0));
    // A marked bar is in its tone's colour, and the error colour is then on the image.
    data.series.front().points.at(1).tone = ui::Tone::Err;
    chart->setData(data);
    scene.settle();
    QImage const marked = scene.window().grabWindow();
    CHECK(runsOf(marked).size() == 1);
    int err = 0;
    for (int count : errPerRow(marked)) {
        err += count;
    }
    CHECK(err > 0);
}

TEST_CASE("qt_quick chart: a Chart to assistive technology, its summary read once", "[qt_quick][chart]") {
    bool const was = QAccessible::isActive();
    QAccessible::setActive(true);
    {
        Scene scene;
        auto chart = scene.backend().createChart(nullptr);
        chart->setData(sample());
        scene.settle();
        QQuickItem& item = Scene::itemOf(*chart);
        QAccessibleInterface const* const node = QAccessible::queryAccessibleInterface(&item);
        REQUIRE(node != nullptr);
        CHECK(node->role() == QAccessible::Chart);
        CHECK(node->text(QAccessible::Name) == QStringLiteral("Three runs, the last one high"));
        CHECK(node->text(QAccessible::Description).isEmpty());
        chart->setAccessibleName("Control chart");
        scene.settle();
        CHECK(node->text(QAccessible::Name) == QStringLiteral("Control chart"));
        CHECK(node->text(QAccessible::Description) == QStringLiteral("Three runs, the last one high"));
    }
    QAccessible::setActive(was);
}

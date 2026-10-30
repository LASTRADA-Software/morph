// SPDX-License-Identifier: Apache-2.0
//
// The chart on Qt Quick: what its Canvas paints, read back from the window at the device pixel ratio the platform
// gives (the suite also runs it with QT_SCALE_FACTOR=2), and what it tells assistive technology.

#include <QAccessible>
#include <QColor>
#include <QImage>
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <morph/ui/view.hpp>
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

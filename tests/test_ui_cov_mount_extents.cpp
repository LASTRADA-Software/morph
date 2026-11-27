// SPDX-License-Identifier: Apache-2.0

// The widths of a table's columns, the window of columns they give at a scroll position, and when a table is windowed.

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <morph/reactive/runtime.hpp>
#include <morph/ui/mount.hpp>
#include <morph/ui/testing/recording_backend.hpp>
#include <morph/ui/view.hpp>
#include <string>
#include <utility>
#include <vector>

#include "test_support.hpp"

namespace ui = morph::ui;

namespace {

using namespace morph::ui;

ColumnExtents extentsOf(std::vector<int> const& widths) {
    ColumnExtents extents;
    extents.assign(widths);
    return extents;
}

}  // namespace

TEST_CASE("cov extents: a measured width only widens a column", "[ui][mount][cov]") {
    ColumnExtents extents = extentsOf({10, 20, 30});
    CHECK(extents.grow(0, 15));
    CHECK(extents.widthOf(0) == 15);
    CHECK(extents.totalWidth() == 65);
    CHECK_FALSE(extents.grow(1, 20));  // equal
    CHECK_FALSE(extents.grow(1, 5));   // narrower
    CHECK(extents.widthOf(1) == 20);
    CHECK_FALSE(extents.grow(3, 99));  // no such column
    CHECK(extents.totalWidth() == 65);
}

TEST_CASE("cov extents: a width the user set is not changed by measurement", "[ui][mount][cov]") {
    ColumnExtents extents = extentsOf({10, 20});
    extents.set(0, 12);
    CHECK_FALSE(extents.grow(0, 40));
    CHECK_FALSE(extents.fit(0, 40));
    CHECK(extents.widthOf(0) == 12);
    extents.set(7, 5);  // no such column: nothing changes
    CHECK(extents.size() == 2);
    CHECK(extents.totalWidth() == 32);
}

TEST_CASE("cov extents: a fitted width may narrow, and a negative width reads as zero", "[ui][mount][cov]") {
    ColumnExtents extents = extentsOf({10, -4, 30});
    CHECK(extents.widthOf(1) == 0);
    CHECK(extents.fit(2, 8));
    CHECK(extents.widthOf(2) == 8);
    CHECK_FALSE(extents.fit(2, 8));
    CHECK(extents.fit(0, -3));
    CHECK(extents.widthOf(0) == 0);
    CHECK_FALSE(extents.fit(3, 1));
    CHECK(extents.totalWidth() == 8);
}

TEST_CASE("cov extents: the columns under a viewport and the offsets before them", "[ui][mount][cov]") {
    ColumnExtents const extents = extentsOf({10, 20, 30, 40});
    CHECK(extents.offsetOf(0) == 0);
    CHECK(extents.offsetOf(2) == 30);
    CHECK(extents.offsetOf(9) == 100);  // past the last: the total
    CHECK(extents.columnsAt(0, 10) == std::pair<std::size_t, std::size_t>{0, 1});
    CHECK(extents.columnsAt(5, 30) == std::pair<std::size_t, std::size_t>{0, 3});
    CHECK(extents.columnsAt(30, 100) == std::pair<std::size_t, std::size_t>{2, 4});
    CHECK(extents.columnsAt(-20, 10) == std::pair<std::size_t, std::size_t>{0, 1});  // a negative offset reads as 0
    CHECK(extents.columnsAt(30, 0) == std::pair<std::size_t, std::size_t>{2, 2});    // no viewport: no columns
    CHECK(extents.columnsAt(500, 10) == std::pair<std::size_t, std::size_t>{4, 4});  // scrolled past the end
}

TEST_CASE("cov extents: the window pins the leading columns and counts the width on each side", "[ui][mount][cov]") {
    ColumnExtents const extents = extentsOf({10, 10, 10, 10, 10, 10});
    ColumnWindow const window = columnWindow(extents, 1, 25, 10, 0);
    CHECK(window.columns == std::vector<std::size_t>{0, 2, 3});  // 25 to 34 lies over columns 2 and 3
    CHECK(window.leading == 10);                                 // column 1 is neither pinned nor shown
    CHECK(window.trailing == 20);
    ColumnWindow const wide = columnWindow(extents, 0, 0, 1000, 2);
    CHECK(wide.columns.size() == 6);
    CHECK(wide.leading == 0);
    CHECK(wide.trailing == 0);
    ColumnWindow const none = columnWindow(extents, 9, 0, 0, 0);  // pinned is clamped to the count
    CHECK(none.columns.size() == 6);
    CHECK(none.leading == 0);
}

TEST_CASE("cov extents: a table is windowed by rows, by columns, or by cells", "[ui][mount][cov]") {
    Virtualization limits;
    limits.threshold = 100;
    limits.columnThreshold = 8;
    limits.cellThreshold = 1000;
    CHECK_FALSE(windowingOf(limits, 10, 4, false).rows);
    CHECK_FALSE(windowingOf(limits, 10, 4, false).columns);
    CHECK(windowingOf(limits, 10, 4, true).rows);    // rows from a server are always windowed
    CHECK(windowingOf(limits, 100, 4, false).rows);  // at the threshold
    CHECK(windowingOf(limits, 10, 9, false).columns);
    Windowing const cells = windowingOf(limits, 90, 12, false);  // 1080 cells
    CHECK(cells.rows);
    CHECK(cells.columns);
    Windowing const noColumns = windowingOf(limits, 90, 0, false);  // no columns: no product to compare
    CHECK_FALSE(noColumns.rows);
    CHECK_FALSE(noColumns.columns);
}

namespace {

using Owner = morph::testing::StepExecutor;

ui::TextInput namedInput(std::string name, bool enabled = true) {
    ui::TextInput spec;
    spec.common.focusId = std::move(name);
    spec.common.enabled = enabled;
    return spec;
}

}  // namespace

TEST_CASE("cov mount focus: focus moves along the chain from a named widget, skipping disabled ones",
          "[ui][mount][cov]") {
    Owner owner;
    morph::reactive::Runtime runtime{owner};
    ui::testing::RecordingBackend backend;
    ui::Mounted view{runtime, backend,
                     ui::column({.children = {ui::textInput(namedInput("a")), ui::textInput(namedInput("b", false)),
                                              ui::textInput(namedInput("c")), ui::textInput(namedInput("d"))}})};
    std::vector<int> const inputs = backend.all("TextInput");
    REQUIRE(inputs.size() == 4);
    CHECK(view.focusNext("a"));
    CHECK(backend.focused() == inputs.at(2));  // b is disabled
    CHECK(view.focusNext("c"));
    CHECK(backend.focused() == inputs.at(3));
    CHECK_FALSE(view.focusNext("d"));  // nothing after the last
    CHECK(backend.focused() == inputs.at(3));
    CHECK(view.focusPrevious("d"));
    CHECK(backend.focused() == inputs.at(2));
    CHECK(view.focusPrevious("c"));
    CHECK(backend.focused() == inputs.at(0));
    CHECK_FALSE(view.focusPrevious("a"));  // nothing before the first
    CHECK_FALSE(view.focusNext("nobody"));
    CHECK_FALSE(view.focusPrevious("nobody"));
}

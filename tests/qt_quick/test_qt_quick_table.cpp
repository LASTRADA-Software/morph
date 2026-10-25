// SPDX-License-Identifier: Apache-2.0
//
// The table on the real Qt Quick engine: cells placed by column, selection by key in each mode, activation without
// toggling, and keys that cross into QML as text, an int64 above 2^53 unchanged.

#include <QAccessible>
#include <QAccessibleInterface>
#include <QColor>
#include <QFont>
#include <QList>
#include <QObject>
#include <QPointF>
#include <QQuickItem>
#include <QSizeF>
#include <QVariantList>
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <morph/reactive/signal.hpp>
#include <morph/ui/backend.hpp>
#include <morph/ui/mount.hpp>
#include <morph/ui/view.hpp>
#include <optional>
#include <ranges>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "qt_quick/kinds.hpp"
#include "qt_quick_harness.hpp"

namespace ui = morph::ui;
namespace detail = morph::qt_quick::detail;
using morph::qt_quick::testing::Scene;

namespace {

struct Rows {
    std::unique_ptr<ui::TableWidget> table;
    std::vector<std::unique_ptr<ui::StackWidget>> rows;
    std::vector<std::unique_ptr<ui::TextWidget>> cells;
};

// A table of two columns whose rows have the given keys, made as the mount makes them.
Rows makeTable(Scene& scene, std::vector<ui::Key> const& keys, ui::SelectionMode mode) {
    Rows made;
    made.table = scene.backend().createTable(nullptr);
    made.table->setColumns({{.label = "Name", .width = ui::Sizing::fixed(12)}, {.label = "Mass"}});
    made.table->setSelectionMode(mode);
    for (ui::Key const& key : keys) {
        auto row = scene.backend().createStack(made.table.get(), ui::Axis::Horizontal);
        made.table->setRowKey(*row, key);
        for (int column = 0; column < 2; ++column) {
            made.cells.push_back(scene.backend().createText(row.get()));
            made.cells.back()->setText("cell");
        }
        made.rows.push_back(std::move(row));
    }
    scene.settle();
    return made;
}

// What a content column adds to its widest cell: a unit of padding on each side. A unit is the theme font's average
// character width, so it differs between platforms and fonts.
qreal columnPadding(Scene& scene) {
    auto* const theme = scene.engine().singletonInstance<QObject*>(QStringLiteral("MorphUi"), QStringLiteral("Theme"));
    REQUIRE(theme != nullptr);
    return 2 * theme->property("unitWidth").toReal();
}

}  // namespace

TEST_CASE("qt_quick table: a row made in a table is a table row, and an int64 key above 2^53 comes back exactly",
          "[qt_quick]") {
    Scene scene;
    std::int64_t const big = (std::int64_t{1} << 53) + 1;
    Rows table = makeTable(scene, {ui::Key{big}, ui::Key{big + 1}}, ui::SelectionMode::Multiple);
    REQUIRE(dynamic_cast<detail::TableRowImpl*>(table.rows.at(0).get()) != nullptr);
    CHECK(Scene::itemOf(*table.rows.at(0)).property("rowKey").toString() == QStringLiteral("i:9007199254740993"));
    std::vector<std::vector<ui::Key>> reported;
    std::vector<ui::Key> activated;
    table.table->setOnSelectionChange([&reported](std::vector<ui::Key> keys, ui::SelectionChange /*change*/) {
        reported.push_back(std::move(keys));
    });
    table.table->setOnActivate([&activated](ui::Key key) { activated.push_back(std::move(key)); });
    scene.click(Scene::itemOf(*table.rows.at(0)));
    REQUIRE(reported.size() == 1);
    CHECK(reported.at(0) == std::vector<ui::Key>{ui::Key{big}});
    scene.click(Scene::itemOf(*table.rows.at(1)), Qt::ControlModifier);
    REQUIRE(reported.size() == 2);
    CHECK(reported.at(1) == std::vector<ui::Key>{ui::Key{big}, ui::Key{big + 1}});
    // Ctrl+click on a selected row takes it out.
    scene.click(Scene::itemOf(*table.rows.at(0)), Qt::ControlModifier);
    CHECK(reported.back() == std::vector<ui::Key>{ui::Key{big + 1}});
    // A double click activates the row without toggling the selection.
    scene.doubleClick(Scene::itemOf(*table.rows.at(1)));
    CHECK(activated == std::vector<ui::Key>{ui::Key{big + 1}});
    CHECK(detail::TableImpl::marked(*detail::baseOf(*table.rows.at(1))));
}

TEST_CASE("qt_quick table: the selection mode limits what the user selects, not what the application marks",
          "[qt_quick]") {
    Scene scene;
    Rows table = makeTable(scene, {ui::Key{std::string{"a"}}, ui::Key{std::string{"b"}}}, ui::SelectionMode::None);
    int reports = 0;
    table.table->setOnSelectionChange(
        [&reports](std::vector<ui::Key> const&, ui::SelectionChange /*change*/) { ++reports; });
    scene.click(Scene::itemOf(*table.rows.at(0)));
    CHECK(reports == 0);
    table.table->setSelection({ui::Key{std::string{"b"}}, ui::Key{std::string{"z"}}});
    CHECK(detail::TableImpl::marked(*detail::baseOf(*table.rows.at(1))));
    CHECK_FALSE(detail::TableImpl::marked(*detail::baseOf(*table.rows.at(0))));
    // A row that arrives with a requested key is marked.
    auto late = scene.backend().createStack(table.table.get(), ui::Axis::Horizontal);
    table.table->setRowKey(*late, ui::Key{std::string{"z"}});
    CHECK(detail::TableImpl::marked(*detail::baseOf(*late)));
    table.table->setSelectionMode(ui::SelectionMode::Single);
    scene.click(Scene::itemOf(*table.rows.at(0)), Qt::ControlModifier);
    CHECK(reports == 1);
    CHECK(detail::TableImpl::marked(*detail::baseOf(*table.rows.at(0))));
    CHECK_FALSE(detail::TableImpl::marked(*detail::baseOf(*table.rows.at(1))));
}

TEST_CASE("qt_quick table: cells sit under their headers, and a hidden cell keeps its column", "[qt_quick]") {
    Scene scene;
    Rows table = makeTable(scene, {ui::Key{std::int64_t{1}}}, ui::SelectionMode::None);
    QQuickItem const& first = Scene::itemOf(*table.cells.at(0));
    QQuickItem const& second = Scene::itemOf(*table.cells.at(1));
    qreal const secondX = second.mapToScene(QPointF{0, 0}).x();
    CHECK(secondX > first.mapToScene(QPointF{0, 0}).x());
    table.cells.at(0)->setVisible(false);
    scene.settle();
    CHECK(second.mapToScene(QPointF{0, 0}).x() == secondX);
}

// Mutation: size a content column from its header alone (the long cell then overlaps the next column).
TEST_CASE("qt_quick table: a content column is as wide as its widest cell, and widens with it", "[qt_quick]") {
    Scene scene;
    auto table = scene.backend().createTable(nullptr);
    table->setColumns({{.label = "A"}, {.label = "B"}});
    auto row = scene.backend().createStack(table.get(), ui::Axis::Horizontal);
    table->setRowKey(*row, ui::Key{std::int64_t{1}});
    auto first = scene.backend().createText(row.get());
    auto second = scene.backend().createText(row.get());
    first->setText("DE00500700100900800700");
    second->setText("0.01");
    scene.settle();
    QQuickItem const& firstItem = Scene::itemOf(*first);
    QQuickItem const& secondItem = Scene::itemOf(*second);
    auto const endOfFirst = [&firstItem] {
        return firstItem.mapToScene(QPointF{0, 0}).x() + firstItem.implicitWidth();
    };
    CHECK(secondItem.mapToScene(QPointF{0, 0}).x() >= endOfFirst());
    first->setText("DE00500700100900800700 and a good deal longer");
    scene.settle();
    CHECK(secondItem.mapToScene(QPointF{0, 0}).x() >= endOfFirst());
}

TEST_CASE("qt_quick table: Enter on a focused row activates it", "[qt_quick]") {
    Scene scene;
    Rows table = makeTable(scene, {ui::Key{std::int64_t{4}}}, ui::SelectionMode::Single);
    std::vector<ui::Key> activated;
    table.table->setOnActivate([&activated](ui::Key key) { activated.push_back(std::move(key)); });
    Scene::itemOf(*table.rows.at(0)).forceActiveFocus();
    scene.key(Qt::Key_Return);
    CHECK(activated == std::vector<ui::Key>{ui::Key{std::int64_t{4}}});
}

// Mutation: never update the cache in `reportCellWidth` (growing a cell's text then shows the same `rowScans`
// delta as a no-op, and the column's reported width stays at its pre-growth value).
TEST_CASE("qt_quick table: growing one row's cell does not rescan every row, and the column still widens",
          "[qt_quick]") {
    Scene scene;
    auto table = scene.backend().createTable(nullptr);
    table->setColumns({{.label = "A"}, {.label = "B"}, {.label = "C"}});
    constexpr int kRows = 300;
    std::vector<std::unique_ptr<ui::StackWidget>> rows;
    std::vector<std::unique_ptr<ui::TextWidget>> cells;
    for (int r = 0; r < kRows; ++r) {
        auto row = scene.backend().createStack(table.get(), ui::Axis::Horizontal);
        table->setRowKey(*row, ui::Key{static_cast<std::int64_t>(r)});
        for (int column = 0; column < 3; ++column) {
            cells.push_back(scene.backend().createText(row.get()));
            cells.back()->setText("cell");
        }
        rows.push_back(std::move(row));
    }
    scene.settle();

    QQuickItem const& tableItem = Scene::itemOf(*table);
    QQuickItem const& firstCell = Scene::itemOf(*cells.at(0));
    qreal const widthBefore = firstCell.implicitWidth();
    // Sanity: the column's width already fits its widest cell after the initial mount (the cache seeded correctly).
    CHECK(tableItem.property("widths").toList().at(0).toReal() >= widthBefore + columnPadding(scene));

    // `rowScans` is a test-only counter the fix adds to MorphTable.qml, incremented once per row visited inside the
    // full-rescan path. Its absence on unfixed code must fail loudly here, not silently read back 0 for both the
    // baseline and the post-change reading (which would make the delta trivially 0 and this test vacuously pass).
    QVariant const baselineVariant = tableItem.property("rowScans");
    REQUIRE(baselineVariant.isValid());
    int const baseline = baselineVariant.toInt();

    cells.at(0)->setText("a very very very very very long cell text");
    scene.settle();

    QVariant const afterVariant = tableItem.property("rowScans");
    REQUIRE(afterVariant.isValid());
    int const delta = afterVariant.toInt() - baseline;
    CHECK(delta == 0);  // the grow path never rescans every row
    qreal const widthAfter = firstCell.implicitWidth();
    CHECK(widthAfter > widthBefore);
    CHECK(tableItem.property("widths").toList().at(0).toReal() >= widthAfter + columnPadding(scene));
}

// Mutation: skip the invalidateColumn/reportCellWidth wiring from TableRowImpl::adopted entirely (a cell's width
// changing then never reaches the table, so this never observes the rescan it measures).
// Mutation: never update the cache in `reportCellWidth`, forcing every grow through the shrink/full-rescan path.
TEST_CASE("qt_quick table: a single-row grow's scan cost does not scale with row count", "[qt_quick]") {
    // What this measures: the number of rows `widestCell` visits for a single-row grow, not wall-clock time. A
    // grown column's cells still all need their x/width updated once the column widens (MorphTableRow.qml's
    // onWidthsChanged: place(), one call per row) -- that O(rows) placement cost is real, necessary, and not what
    // this fix addresses (virtualizing which rows exist at all is a separate, larger concern). What the fix
    // does address is the O(rows) *scan* needed merely to compute the new width, which this isolates by reading
    // `rowScans`'s delta instead of timing the whole settle.
    auto const buildAndGrow = [](int rowCount) -> int {
        Scene scene;
        auto table = scene.backend().createTable(nullptr);
        table->setColumns({{.label = "A"}, {.label = "B"}, {.label = "C"}});
        std::vector<std::unique_ptr<ui::StackWidget>> rows;
        std::vector<std::unique_ptr<ui::TextWidget>> cells;
        for (int r = 0; r < rowCount; ++r) {
            auto row = scene.backend().createStack(table.get(), ui::Axis::Horizontal);
            table->setRowKey(*row, ui::Key{static_cast<std::int64_t>(r)});
            for (int column = 0; column < 3; ++column) {
                cells.push_back(scene.backend().createText(row.get()));
                cells.back()->setText("cell");
            }
            rows.push_back(std::move(row));
        }
        scene.settle();
        QQuickItem const& tableItem = Scene::itemOf(*table);
        int const baseline = tableItem.property("rowScans").toInt();
        cells.at(0)->setText("a very very very very very long cell text, toggled");
        scene.settle();
        return tableItem.property("rowScans").toInt() - baseline;
    };

    int const small = buildAndGrow(40);
    int const large = buildAndGrow(420);
    std::ostringstream message;
    message << "40 rows: " << small << " scans, 420 rows: " << large << " scans";
    INFO(message.str());
    CHECK(small == 0);
    CHECK(large == 0);
}

TEST_CASE("qt_quick table: an empty table resolves a content column to its header's width, without throwing",
          "[qt_quick]") {
    Scene scene;
    auto table = scene.backend().createTable(nullptr);
    table->setColumns({{.label = "A"}, {.label = "B"}});
    scene.settle();
    QQuickItem const& tableItem = Scene::itemOf(*table);
    QVariantList const widths = tableItem.property("widths").toList();
    REQUIRE(widths.size() == 2);
    CHECK(widths.at(0).toReal() > 0);
    CHECK(widths.at(1).toReal() > 0);
}

TEST_CASE("qt_quick table: a cell's text shrinking rescans its column and keeps the correct width", "[qt_quick]") {
    Scene scene;
    auto table = scene.backend().createTable(nullptr);
    table->setColumns({{.label = "A"}});
    auto row1 = scene.backend().createStack(table.get(), ui::Axis::Horizontal);
    table->setRowKey(*row1, ui::Key{std::int64_t{1}});
    auto cell1 = scene.backend().createText(row1.get());
    cell1->setText("a very very very very very long cell text");
    auto row2 = scene.backend().createStack(table.get(), ui::Axis::Horizontal);
    table->setRowKey(*row2, ui::Key{std::int64_t{2}});
    auto cell2 = scene.backend().createText(row2.get());
    cell2->setText("short");
    scene.settle();
    QQuickItem const& tableItem = Scene::itemOf(*table);
    qreal const widthBefore = tableItem.property("widths").toList().at(0).toReal();

    cell1->setText("short too");
    scene.settle();
    qreal const widthAfter = tableItem.property("widths").toList().at(0).toReal();
    CHECK(widthAfter < widthBefore);
    CHECK(widthAfter >= Scene::itemOf(*cell1).implicitWidth() + columnPadding(scene));
}

TEST_CASE("qt_quick table: removing a row drops its cell's contribution to the column width", "[qt_quick]") {
    Scene scene;
    auto table = scene.backend().createTable(nullptr);
    table->setColumns({{.label = "A"}});
    auto row1 = scene.backend().createStack(table.get(), ui::Axis::Horizontal);
    table->setRowKey(*row1, ui::Key{std::int64_t{1}});
    auto cell1 = scene.backend().createText(row1.get());
    cell1->setText("a very very very very very long cell text");
    auto row2 = scene.backend().createStack(table.get(), ui::Axis::Horizontal);
    table->setRowKey(*row2, ui::Key{std::int64_t{2}});
    auto cell2 = scene.backend().createText(row2.get());
    cell2->setText("short");
    scene.settle();
    QQuickItem const& tableItem = Scene::itemOf(*table);
    qreal const widthBefore = tableItem.property("widths").toList().at(0).toReal();

    row1.reset();
    scene.settle();
    qreal const widthAfter = tableItem.property("widths").toList().at(0).toReal();
    CHECK(widthAfter < widthBefore);
}

TEST_CASE("qt_quick table: a hidden cell does not count toward the column width, and shows again when shown",
          "[qt_quick]") {
    Scene scene;
    auto table = scene.backend().createTable(nullptr);
    table->setColumns({{.label = "A"}});
    auto row1 = scene.backend().createStack(table.get(), ui::Axis::Horizontal);
    table->setRowKey(*row1, ui::Key{std::int64_t{1}});
    auto cell1 = scene.backend().createText(row1.get());
    cell1->setText("a very very very very very long cell text");
    auto row2 = scene.backend().createStack(table.get(), ui::Axis::Horizontal);
    table->setRowKey(*row2, ui::Key{std::int64_t{2}});
    auto cell2 = scene.backend().createText(row2.get());
    cell2->setText("short");
    scene.settle();
    QQuickItem const& tableItem = Scene::itemOf(*table);
    qreal const widthBefore = tableItem.property("widths").toList().at(0).toReal();

    cell1->setVisible(false);
    scene.settle();
    qreal const widthHidden = tableItem.property("widths").toList().at(0).toReal();
    CHECK(widthHidden < widthBefore);

    cell1->setVisible(true);
    scene.settle();
    qreal const widthShown = tableItem.property("widths").toList().at(0).toReal();
    CHECK(widthShown == widthBefore);
}

// Mutation: write no tone on the row (the backgrounds stay clear).
TEST_CASE("qt_quick table: a toned row's background fills with the tone's bg and keeps its bar when selected",
          "[qt_quick]") {
    Scene scene;
    Rows table = makeTable(scene, {ui::Key{std::string{"a"}}, ui::Key{std::string{"b"}}, ui::Key{std::string{"c"}}},
                           ui::SelectionMode::Single);
    table.table->setRowTone(*table.rows.at(0), ui::Tone::Ok);
    table.table->setRowTone(*table.rows.at(1), ui::Tone::Err);
    scene.settle();
    auto* const theme = scene.engine().singletonInstance<QObject*>(QStringLiteral("MorphUi"), QStringLiteral("Theme"));
    REQUIRE(theme != nullptr);
    auto const part = [](ui::StackWidget const& row, char const* name) -> QQuickItem& {
        auto* const background = Scene::itemOf(row).property("backgroundItem").value<QQuickItem*>();
        REQUIRE(background != nullptr);
        auto* const found = background->findChild<QQuickItem*>(QString::fromLatin1(name));
        REQUIRE(found != nullptr);
        return *found;
    };
    CHECK(part(*table.rows.at(0), "fill").property("color").value<QColor>() ==
          theme->property("successBg").value<QColor>());
    CHECK(part(*table.rows.at(1), "fill").property("color").value<QColor>() ==
          theme->property("errorBg").value<QColor>());
    CHECK(part(*table.rows.at(0), "toneBar").isVisible());
    CHECK(part(*table.rows.at(0), "toneBar").property("color").value<QColor>() ==
          theme->property("successDot").value<QColor>());
    CHECK_FALSE(part(*table.rows.at(2), "toneBar").isVisible());
    // Selected, the row shows the selection and keeps its bar.
    table.table->setSelection({ui::Key{std::string{"a"}}});
    scene.settle();
    CHECK(part(*table.rows.at(0), "fill").property("color").value<QColor>() ==
          theme->property("selection").value<QColor>());
    CHECK(part(*table.rows.at(0), "toneBar").isVisible());
    // No tone again.
    table.table->setRowTone(*table.rows.at(1), std::nullopt);
    scene.settle();
    CHECK_FALSE(part(*table.rows.at(1), "toneBar").isVisible());
}

namespace {

struct VirtualRow {
    std::int64_t id = 0;
    std::string name;
};

std::vector<ui::Node> virtualRowCell(morph::reactive::Signal<VirtualRow> const& row) {
    return {ui::text({.text = [&row] { return row.get().name; }})};
}

}  // namespace

TEST_CASE("qt_quick table: a virtualized table realizes only the window, reporting it to C++ as it scrolls",
          "[qt_quick]") {
    Scene scene;
    std::vector<VirtualRow> const many = [] {
        std::vector<VirtualRow> rows;
        for (std::int64_t i = 0; i < 20; ++i) {
            rows.push_back(VirtualRow{.id = i, .name = "row-" + std::to_string(i)});
        }
        return rows;
    }();
    ui::Mounted const mounted{
        scene.runtime(), scene.backend(),
        ui::table<VirtualRow>(
            {{.label = "Name"}}, [&many] { return many; }, [](VirtualRow const& row) { return ui::Key{row.id}; },
            virtualRowCell,
            {.virtualize = {.threshold = 5, .overscan = 1}, .common = {.layout = {.height = ui::Sizing::fixed(10)}}})};
    scene.settle();

    QQuickItem const& tableItem = Scene::itemOf(mounted.root());
    auto* const flick = tableItem.findChild<QQuickItem*>(QStringLiteral("flick"));
    REQUIRE(flick != nullptr);

    // A real Flickable's height, from the fixed layout height given above, bounds how many rows the bootstrap
    // window and a later scroll can realize -- unlike RecordingBackend's own virtualization test (test_ui_table
    // .cpp), which has no real geometry to derive a range from and so only exercises mount.hpp's own windowing
    // logic against an explicit scrollTable() call, this is the real Qt Quick geometry path end to end.
    CHECK(tableItem.property("totalRows").toInt() == 20);
    CHECK(tableItem.property("realizedCount").toInt() < 20);

    // Scrolling moves the window: a position far down the content realizes rows near the end, not the start.
    flick->setProperty("contentY", flick->property("contentHeight").toReal() * 0.9);
    scene.settle();
    int const scrolledFirst = tableItem.property("realizedFirst").toInt();
    CHECK(scrolledFirst > 0);
}

TEST_CASE("qt_quick table: a virtualized table's column width never shrinks as a wide row dewindows", "[qt_quick]") {
    Scene scene;
    std::vector<VirtualRow> const many = [] {
        std::vector<VirtualRow> rows;
        // Row 0 has by far the widest name; every other row is short. Scrolling away from row 0 must not shrink
        // the column back down to the short rows' width (the "monotonic, not shrinking" trade-off this design
        // accepts in exchange for no jitter -- see MorphTable.qml's invalidateAllColumns).
        rows.push_back(VirtualRow{.id = 0, .name = "a very very very very very very long row name indeed"});
        for (std::int64_t i = 1; i < 20; ++i) {
            rows.push_back(VirtualRow{.id = i, .name = "short"});
        }
        return rows;
    }();
    ui::Mounted const mounted{
        scene.runtime(), scene.backend(),
        ui::table<VirtualRow>(
            {{.label = "Name"}}, [&many] { return many; }, [](VirtualRow const& row) { return ui::Key{row.id}; },
            virtualRowCell,
            {.virtualize = {.threshold = 5, .overscan = 1}, .common = {.layout = {.height = ui::Sizing::fixed(10)}}})};
    scene.settle();

    QQuickItem const& tableItem = Scene::itemOf(mounted.root());
    auto* const flick = tableItem.findChild<QQuickItem*>(QStringLiteral("flick"));
    REQUIRE(flick != nullptr);
    qreal const widthWithWideRow = tableItem.property("widths").toList().at(0).toReal();

    // Scroll far enough that row 0 dewindows.
    flick->setProperty("contentY", flick->property("contentHeight").toReal() * 0.9);
    scene.settle();
    REQUIRE(tableItem.property("realizedFirst").toInt() > 0);
    CHECK(tableItem.property("widths").toList().at(0).toReal() == widthWithWideRow);
}

namespace {

// The Theme singleton's unit width and height.
QSizeF units(Scene& scene) {
    auto* const theme = scene.engine().singletonInstance<QObject*>(QStringLiteral("MorphUi"), QStringLiteral("Theme"));
    REQUIRE(theme != nullptr);
    return {theme->property("unitWidth").toReal(), theme->property("unitHeight").toReal()};
}

// The cell inset a row places its cells by: `Theme.spaceOf(2)`.
constexpr qreal kCellInset = 8;

}  // namespace

// Mutation: give every cell its column's width in MorphTableRow.place (the badge stretches across its column).
TEST_CASE("qt_quick table: a badge keeps its own width at its column's start", "[qt_quick]") {
    Scene scene;
    auto table = scene.backend().createTable(nullptr);
    table->setColumns({{.label = "State", .width = ui::Sizing::fixed(30)}, {.label = "Name"}});
    auto row = scene.backend().createStack(table.get(), ui::Axis::Horizontal);
    table->setRowKey(*row, ui::Key{std::int64_t{1}});
    auto badge = scene.backend().createBadge(row.get());
    badge->setText("OK");
    auto name = scene.backend().createText(row.get());
    name->setText("pH");
    scene.settle();

    QQuickItem const& badgeItem = Scene::itemOf(*badge);
    qreal const column = 30 * units(scene).width();
    CHECK(badgeItem.width() < column - (2 * kCellInset));
    CHECK(badgeItem.width() == badgeItem.implicitWidth());
    CHECK(badgeItem.x() == kCellInset);
}

// Mutation: drop place()'s textAlign write (the text and the field read AlignLeft).
TEST_CASE("qt_quick table: an end column draws its text, its field's text and its header label at its end",
          "[qt_quick]") {
    Scene scene;
    auto table = scene.backend().createTable(nullptr);
    table->setColumns({{.label = "Area", .width = ui::Sizing::fixed(12), .align = ui::Align::End},
                       {.label = "Result", .width = ui::Sizing::fixed(12), .align = ui::Align::End},
                       {.label = "Name"}});
    auto row = scene.backend().createStack(table.get(), ui::Axis::Horizontal);
    table->setRowKey(*row, ui::Key{std::int64_t{1}});
    auto area = scene.backend().createText(row.get());
    area->setText("0.21");
    auto result = scene.backend().createTextInput(row.get(), ui::TextInputMode::SingleLine);
    result->setText("7.4");
    auto name = scene.backend().createText(row.get());
    name->setText("pH");
    scene.settle();

    auto const alignment = [](QQuickItem const& item) {
        return item.property("horizontalAlignment").toInt() & Qt::AlignHorizontal_Mask;
    };
    CHECK(alignment(Scene::controlOf(*area)) == Qt::AlignRight);
    CHECK(alignment(Scene::controlOf(*result)) == Qt::AlignRight);
    CHECK(alignment(Scene::controlOf(*name)) == Qt::AlignLeft);
    // Tabular figures, so digits line up across rows.
    CHECK(Scene::controlOf(*area).property("font").value<QFont>().featureValue(QFont::Tag{"tnum"}) == 1);
    QList<QQuickItem*> const labels = Scene::itemsOf(Scene::itemOf(*table), "QQuickLabel");
    auto const header = std::ranges::find_if(
        labels, [](QQuickItem const* label) { return label->property("text").toString() == QStringLiteral("Area"); });
    REQUIRE(header != labels.end());
    CHECK(alignment(**header) == Qt::AlignRight);
}

// Mutation: leave every cell at the row's top with the tallest cell's height (the text sits above the select's
// centre).
TEST_CASE("qt_quick table: a text cell beside a taller select is centred in its row", "[qt_quick]") {
    Scene scene;
    auto table = scene.backend().createTable(nullptr);
    table->setColumns({{.label = "Test"}, {.label = "When"}});
    auto row = scene.backend().createStack(table.get(), ui::Axis::Horizontal);
    table->setRowKey(*row, ui::Key{std::int64_t{1}});
    auto text = scene.backend().createText(row.get());
    text->setText("Assay");
    auto select = scene.backend().createSelect(row.get(), ui::SelectStyle::Dropdown);
    select->setOptions({{.key = ui::Key{std::int64_t{1}}, .label = "Initial"}});
    scene.settle();

    QQuickItem const& textItem = Scene::itemOf(*text);
    QQuickItem const& rowItem = Scene::itemOf(*row);
    REQUIRE(Scene::itemOf(*select).implicitHeight() > textItem.implicitHeight());
    qreal const above = textItem.y();
    qreal const below = rowItem.height() - (textItem.y() + textItem.height());
    CHECK(above > 0);
    CHECK(std::abs(above - below) <= 1.0);
}

namespace {

// A table @p width units wide: two fixed columns of 40 and a stretch column headed `Analyte`, with one row.
struct WideTable {
    WideTable(Scene& scene, int width) {
        table = scene.backend().createTable(nullptr);
        table->setLayout({.width = ui::Sizing::fixed(width), .height = {}});
        table->setColumns({{.label = "Sample", .width = ui::Sizing::fixed(40)},
                           {.label = "Lot", .width = ui::Sizing::fixed(40)},
                           {.label = "Analyte", .width = ui::Sizing::stretch(1)}});
        row = scene.backend().createStack(table.get(), ui::Axis::Horizontal);
        table->setRowKey(*row, ui::Key{std::int64_t{1}});
        for (auto const* const label : {"S-1", "L-1", "Assay"}) {
            cells.push_back(scene.backend().createText(row.get()));
            cells.back()->setText(label);
        }
        scene.settle();
    }

    std::unique_ptr<ui::TableWidget> table;
    std::unique_ptr<ui::StackWidget> row;
    std::vector<std::unique_ptr<ui::TextWidget>> cells;
};

}  // namespace

// Mutation: give a stretch column only what the others leave, with no floor (it is 0 wide and the cells overlap), or
// make `hflick` never interactive.
TEST_CASE("qt_quick table: a table wider than its area keeps every column and scrolls sideways", "[qt_quick]") {
    Scene scene;
    WideTable const wide{scene, 60};
    QQuickItem const& tableItem = Scene::itemOf(*wide.table);
    QVariantList const widths = tableItem.property("widths").toList();
    REQUIRE(widths.size() == 3);
    qreal const unit = units(scene).width();
    CHECK(widths.at(2).toReal() >= 8 * unit);
    CHECK(widths.at(2).toReal() >= tableItem.property("columnsWidth").toReal() - (80 * unit) - 1);
    for (std::size_t index = 1; index < wide.cells.size(); ++index) {
        QQuickItem const& before = Scene::itemOf(*wide.cells.at(index - 1));
        QQuickItem const& after = Scene::itemOf(*wide.cells.at(index));
        CHECK(after.mapToScene(QPointF{0, 0}).x() >= before.mapToScene(QPointF{before.width(), 0}).x());
        CHECK(after.width() > 0);
    }
    auto const* const hflick = tableItem.findChild<QQuickItem*>(QStringLiteral("hflick"));
    REQUIRE(hflick != nullptr);
    CHECK(hflick->property("interactive").toBool());
    qreal const sum = widths.at(0).toReal() + widths.at(1).toReal() + widths.at(2).toReal();
    CHECK(std::abs(hflick->property("contentWidth").toReal() - sum) < 0.5);

    Scene roomy;
    WideTable const fits{roomy, 200};
    auto const* const fitting = Scene::itemOf(*fits.table).findChild<QQuickItem*>(QStringLiteral("hflick"));
    REQUIRE(fitting != nullptr);
    CHECK_FALSE(fitting->property("interactive").toBool());
}

// Mutation: size `flick` to its rows whether or not the table is virtualized (the table's rows overflow its height
// and `flick` cannot scroll).
TEST_CASE("qt_quick table: a virtualized table keeps the height it is given, and its rows scroll inside it",
          "[qt_quick]") {
    Scene scene;
    auto const many = std::views::iota(std::int64_t{0}, std::int64_t{40}) |
                      std::views::transform([](std::int64_t index) {
                          return VirtualRow{.id = index, .name = "row-" + std::to_string(index)};
                      }) |
                      std::ranges::to<std::vector>();
    ui::Mounted const mounted{
        scene.runtime(), scene.backend(),
        ui::table<VirtualRow>(
            {{.label = "Name"}}, [&many] { return std::vector<VirtualRow>{many}; },
            [](VirtualRow const& row) { return ui::Key{row.id}; }, virtualRowCell,
            {.virtualize = {.threshold = 5, .overscan = 1}, .common = {.layout = {.height = ui::Sizing::fixed(10)}}})};
    scene.settle();

    // The roots are a column, so the table is a fixed-height child of one.
    QQuickItem const* const tableItem = &Scene::itemOf(mounted.root());
    auto* const flick = tableItem->findChild<QQuickItem*>(QStringLiteral("flick"));
    REQUIRE(flick != nullptr);
    qreal const given = 10 * units(scene).height();
    CHECK(std::abs(tableItem->height() - given) < 1.0);
    // The rows' viewport is what the header leaves of that height, whatever the realized rows measure.
    CHECK(flick->y() > 0);
    CHECK(std::abs(flick->y() + flick->height() - given) < 1.0);
    CHECK(flick->property("contentHeight").toReal() > flick->height());
    CHECK(flick->property("interactive").toBool());
}

// Mutation: drop MorphTable's activeFocusOnTab (Tab never reaches a row) or its ownKey (Down and Space do nothing).
TEST_CASE("qt_quick table: Tab focuses a table of text cells, Down then Space selects the second row, Enter activates",
          "[qt_quick]") {
    Scene scene;
    Rows table = makeTable(scene, {ui::Key{std::int64_t{1}}, ui::Key{std::int64_t{2}}, ui::Key{std::int64_t{3}}},
                           ui::SelectionMode::Multiple);
    std::vector<std::pair<std::vector<ui::Key>, ui::SelectionChange>> reported;
    std::vector<ui::Key> activated;
    table.table->setOnSelectionChange([&reported](std::vector<ui::Key> keys, ui::SelectionChange change) {
        reported.emplace_back(std::move(keys), change);
    });
    table.table->setOnActivate([&activated](ui::Key key) { activated.push_back(std::move(key)); });
    scene.key(Qt::Key_Tab);
    CHECK(Scene::itemOf(*table.rows.at(0)).hasActiveFocus());
    scene.key(Qt::Key_Down);
    CHECK(Scene::itemOf(*table.rows.at(1)).hasActiveFocus());
    CHECK(reported.empty());
    scene.key(Qt::Key_Space);
    REQUIRE(reported.size() == 1);
    CHECK(reported.back().first == std::vector<ui::Key>{ui::Key{std::int64_t{2}}});
    CHECK(reported.back().second == ui::SelectionChange::Extend);
    CHECK(detail::TableImpl::marked(*detail::baseOf(*table.rows.at(1))));
    scene.key(Qt::Key_Return);
    CHECK(activated == std::vector<ui::Key>{ui::Key{std::int64_t{2}}});
}

namespace {

// The select column's box on a row, and the select-all box over the table.
QQuickItem& selectBoxOf(ui::Widget const& row) {
    auto* const box = Scene::itemOf(row).findChild<QQuickItem*>(QStringLiteral("selectBox"));
    REQUIRE(box != nullptr);
    return *box;
}

QQuickItem& selectAllOf(ui::Widget const& table) {
    auto* const box = Scene::itemOf(table).findChild<QQuickItem*>(QStringLiteral("selectAll"));
    REQUIRE(box != nullptr);
    return *box;
}

// A multiple table of three rows with a select column, labelled by their samples.
Rows selectTable(Scene& scene) {
    Rows made;
    made.table = scene.backend().createTable(nullptr);
    made.table->setColumns({{.label = "Sample", .width = ui::Sizing::fixed(12)}});
    made.table->setSelectionMode(ui::SelectionMode::Multiple);
    made.table->setSelectColumn(true);
    made.table->setSelectAllLabel("Select all on this page");
    for (std::int64_t id = 1; id <= 3; ++id) {
        auto row = scene.backend().createStack(made.table.get(), ui::Axis::Horizontal);
        made.cells.push_back(scene.backend().createText(row.get()));
        made.cells.back()->setText("S-" + std::to_string(id));
        made.table->setRowKey(*row, ui::Key{id});
        made.table->setRowLabel(*row, "S-" + std::to_string(id));
        made.rows.push_back(std::move(row));
    }
    scene.settle();
    return made;
}

}  // namespace

// Mutation: drop setSelectColumn's write (the select-all box is not shown, nor a row's box).
TEST_CASE("qt_quick table: the select column's boxes show and change the selection", "[qt_quick]") {
    Scene scene;
    Rows table = selectTable(scene);
    std::vector<std::pair<std::vector<ui::Key>, ui::SelectionChange>> reported;
    table.table->setOnSelectionChange([&reported](std::vector<ui::Key> keys, ui::SelectionChange change) {
        reported.emplace_back(std::move(keys), change);
    });
    QQuickItem& selectAll = selectAllOf(*table.table);
    REQUIRE(selectAll.isVisible());

    table.table->setSelection({ui::Key{std::int64_t{1}}, ui::Key{std::int64_t{2}}});
    scene.settle();
    CHECK(selectAll.property("checkState").toInt() == Qt::PartiallyChecked);

    scene.click(selectAll);
    REQUIRE(!reported.empty());
    CHECK(reported.back().second == ui::SelectionChange::Extend);
    CHECK(selectAll.property("checkState").toInt() == Qt::Checked);
    scene.click(selectAll);
    CHECK(reported.back().first.empty());
    CHECK(selectAll.property("checkState").toInt() == Qt::Unchecked);

    // A row's box toggles its row and keeps the others.
    table.table->setSelection({ui::Key{std::int64_t{1}}});
    scene.settle();
    scene.click(selectBoxOf(*table.rows.at(2)));
    CHECK(reported.back().first == std::vector<ui::Key>{ui::Key{std::int64_t{1}}, ui::Key{std::int64_t{3}}});
    CHECK(selectBoxOf(*table.rows.at(2)).property("checked").toBool());
    CHECK(selectBoxOf(*table.rows.at(0)).property("checked").toBool());

    // The box carries its row's name, and the select-all box the table's label.
    bool const wasActive = QAccessible::isActive();
    QAccessible::setActive(true);
    QAccessibleInterface* const rowBox = QAccessible::queryAccessibleInterface(&selectBoxOf(*table.rows.at(1)));
    QAccessibleInterface* const allBox = QAccessible::queryAccessibleInterface(&selectAll);
    REQUIRE(rowBox != nullptr);
    REQUIRE(allBox != nullptr);
    CHECK(rowBox->text(QAccessible::Name) == QStringLiteral("S-2"));
    CHECK(allBox->text(QAccessible::Name) == QStringLiteral("Select all on this page"));
    QAccessible::setActive(wasActive);
}

// Mutation: let the row's box keep its own state rather than show `selected` (it stays checked over a row the
// document did not select).
TEST_CASE("qt_quick table: a row's select box under a bound selection that refuses the click stays clear",
          "[qt_quick]") {
    Scene scene;
    struct Sample {
        std::int64_t id = 0;
        bool operator==(Sample const&) const = default;
    };
    std::vector<Sample> const samples{{.id = 1}, {.id = 2}};
    ui::Mounted const mounted{
        scene.runtime(), scene.backend(),
        ui::table<Sample>(
            {{.label = "Sample"}}, [&samples] { return samples; },
            [](Sample const& sample) { return ui::Key{sample.id}; },
            [](morph::reactive::Signal<Sample> const&) { return std::vector<ui::Node>{ui::text({.text = "S"})}; },
            {.selectionMode = ui::SelectionMode::Multiple,
             .selection = [] { return std::vector<ui::Key>{}; },
             .onSelectionChange = [](std::vector<ui::Key> const&, ui::SelectionChange) {},
             .selectColumn = true})};
    scene.settle();
    auto const* const core = dynamic_cast<detail::ContainerCore const*>(&mounted.root());
    REQUIRE(core != nullptr);
    auto const& rows = core->children();
    REQUIRE(rows.size() == 2);
    auto* const box = rows.at(1)->item()->findChild<QQuickItem*>(QStringLiteral("selectBox"));
    REQUIRE(box != nullptr);
    scene.click(*box);
    scene.settle();
    CHECK_FALSE(box->property("checked").toBool());
    CHECK_FALSE(detail::TableImpl::marked(*rows.at(1)));
}

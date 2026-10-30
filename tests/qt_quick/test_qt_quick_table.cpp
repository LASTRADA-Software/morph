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
#include <morph/qt_quick/theme.hpp>
#include <morph/reactive/signal.hpp>
#include <morph/ui/backend.hpp>
#include <morph/ui/mount.hpp>
#include <morph/ui/theme.hpp>
#include <morph/ui/view.hpp>
#include <optional>
#include <ranges>
#include <sstream>
#include <stdexcept>
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

// What a column adds to its content: the table cell padding on each side.
qreal columnPadding(Scene& scene) {
    auto* const theme = scene.engine().singletonInstance<QObject*>(QStringLiteral("MorphUi"), QStringLiteral("Theme"));
    REQUIRE(theme != nullptr);
    return 2 * theme->property("tableCellPaddingX").toReal();
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

// Mutation: measure a mono column in `Theme.unitWidth` (the identifier is cut short), or leave `singleLine` unset
// (the 5-unit text wraps onto several lines).
TEST_CASE("qt_quick table: a fixed mono column holds its identifiers whole, and a column without wrap keeps one line",
          "[qt_quick]") {
    Scene scene;
    // A mono face wider than the body font's average character, as a theme with a larger mono size draws.
    ui::ThemeValues wider = ui::themeValues();
    wider.dimensions.insert_or_assign("font.size.mono", 18.0);
    REQUIRE(morph::qt_quick::applyTheme(scene.engine(), wider).empty());
    auto table = scene.backend().createTable(nullptr);
    table->setColumns({{.label = "Sample", .width = ui::Sizing::fixed(11), .role = ui::TextRole::Mono},
                       {.label = "Assay", .width = ui::Sizing::fixed(5), .wrap = false},
                       {.label = "Note", .width = ui::Sizing::fixed(5)}});
    auto row = scene.backend().createStack(table.get(), ui::Axis::Horizontal);
    table->setRowKey(*row, ui::Key{std::int64_t{1}});
    auto id = scene.backend().createText(row.get());
    id->setRole(ui::TextRole::Mono);
    id->setText("S-26-104170");
    auto unwrapped = scene.backend().createText(row.get());
    unwrapped->setText("99.4 % LC");
    auto wrapped = scene.backend().createText(row.get());
    wrapped->setText("99.4 % LC");
    scene.settle();
    QObject const& idLabel = Scene::controlOf(*id);
    CHECK_FALSE(Scene::itemOf(*id).property("elided").toBool());
    CHECK(idLabel.property("lineCount").toInt() == 1);
    QObject const& unwrappedLabel = Scene::controlOf(*unwrapped);
    CHECK(unwrappedLabel.property("lineCount").toInt() == 1);
    CHECK(Scene::itemOf(*unwrapped).property("elided").toBool());
    CHECK(Scene::controlOf(*wrapped).property("lineCount").toInt() > 1);
    // The row is one line of the tallest cell, not the wrapped text's several.
    CHECK(Scene::itemOf(*unwrapped).height() < Scene::itemOf(*wrapped).height());
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

namespace {

// The colour a tone's fill gives at half strength over a surface: what a toned row is tinted with.
QColor halfOver(QColor const& surface, QColor const& fill) {
    float const alpha = fill.alphaF() / 2.0F;
    auto const mix = [alpha](float below, float above) { return (below * (1.0F - alpha)) + (above * alpha); };
    return QColor::fromRgbF(mix(surface.redF(), fill.redF()), mix(surface.greenF(), fill.greenF()),
                            mix(surface.blueF(), fill.blueF()));
}

bool sameColour(QColor const& left, QColor const& right) {
    constexpr float kTolerance = 0.01F;
    return std::abs(left.redF() - right.redF()) < kTolerance &&
           std::abs(left.greenF() - right.greenF()) < kTolerance &&
           std::abs(left.blueF() - right.blueF()) < kTolerance &&
           std::abs(left.alphaF() - right.alphaF()) < kTolerance;
}

QQuickItem& partOf(ui::StackWidget const& row, char const* name) {
    auto* const background = Scene::itemOf(row).property("backgroundItem").value<QQuickItem*>();
    REQUIRE(background != nullptr);
    auto* const found = background->findChild<QQuickItem*>(QString::fromLatin1(name));
    REQUIRE(found != nullptr);
    return *found;
}

}  // namespace

// Mutation: write no tone on the row (no tint); draw the tone's bar again (a `toneBar` is found); draw the selection
// fill whatever the select column (the second table's selected row is filled); leave out the selection bar.
TEST_CASE(
    "qt_quick table: a toned row is tinted with no bar, a selected row has the accent's bar and, without a "
    "select column, the selection fill",
    "[qt_quick]") {
    Scene scene;
    Rows table = makeTable(scene, {ui::Key{std::string{"a"}}, ui::Key{std::string{"b"}}, ui::Key{std::string{"c"}}},
                           ui::SelectionMode::Single);
    table.table->setRowTone(*table.rows.at(0), ui::Tone::Ok);
    table.table->setRowTone(*table.rows.at(1), ui::Tone::Err);
    scene.settle();
    auto* const theme = scene.engine().singletonInstance<QObject*>(QStringLiteral("MorphUi"), QStringLiteral("Theme"));
    REQUIRE(theme != nullptr);
    auto const colorOf = [](QQuickItem const& item) { return item.property("color").value<QColor>(); };
    QColor const surface = theme->property("surface").value<QColor>();
    CHECK(sameColour(colorOf(partOf(*table.rows.at(0), "tint")),
                     halfOver(surface, theme->property("successBg").value<QColor>())));
    CHECK(sameColour(colorOf(partOf(*table.rows.at(1), "tint")),
                     halfOver(surface, theme->property("errorBg").value<QColor>())));
    CHECK(partOf(*table.rows.at(0), "tint").isVisible());
    CHECK_FALSE(partOf(*table.rows.at(2), "tint").isVisible());
    // A tone alone draws no bar.
    for (auto const& row : table.rows) {
        CHECK_FALSE(partOf(*row, "selectionBar").isVisible());
        CHECK(Scene::itemOf(*row)
                  .property("backgroundItem")
                  .value<QQuickItem*>()
                  ->findChild<QQuickItem*>(QStringLiteral("toneBar")) == nullptr);
    }
    // Selected, the row has the accent's bar and the selection fill over its tint.
    table.table->setSelection({ui::Key{std::string{"a"}}});
    scene.settle();
    CHECK(partOf(*table.rows.at(0), "selectionBar").isVisible());
    CHECK(partOf(*table.rows.at(0), "selectionBar").property("width").toReal() == 3);
    CHECK(sameColour(colorOf(partOf(*table.rows.at(0), "selectionBar")), theme->property("accent").value<QColor>()));
    CHECK(sameColour(colorOf(partOf(*table.rows.at(0), "fill")), theme->property("selection").value<QColor>()));
    CHECK(partOf(*table.rows.at(0), "tint").isVisible());
    // No tone again.
    table.table->setRowTone(*table.rows.at(1), std::nullopt);
    scene.settle();
    CHECK_FALSE(partOf(*table.rows.at(1), "tint").isVisible());
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

// Mutation: take the padding out of the column's declared width in MorphTable.columnWidths (the column is then n units
// wide and its cell n units less twice the inset).
TEST_CASE("qt_quick table: a fixed column's width is its units plus the cell padding on both sides", "[qt_quick]") {
    Scene scene;
    auto table = scene.backend().createTable(nullptr);
    table->setColumns({{.label = "Age", .width = ui::Sizing::fixed(14)}, {.label = "Name"}});
    auto row = scene.backend().createStack(table.get(), ui::Axis::Horizontal);
    table->setRowKey(*row, ui::Key{std::int64_t{1}});
    auto age = scene.backend().createText(row.get());
    age->setText("22 min overdue");
    auto name = scene.backend().createText(row.get());
    name->setText("pH");
    scene.settle();

    qreal const content = 14 * units(scene).width();
    qreal const column = Scene::itemOf(*table).property("widths").toList().at(0).toReal();
    CHECK(column == content + columnPadding(scene));
    // The cell is exactly the declared content width, so text that is that wide stays on its line.
    CHECK(Scene::itemOf(*age).width() == content);
}

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
    CHECK(widths.at(2).toReal() >=
          tableItem.property("columnsWidth").toReal() - (80 * unit) - (2 * columnPadding(scene)) - 1);
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
    std::vector<VirtualRow> many;
    many.reserve(40);
    for (std::int64_t index = 0; index < 40; ++index) {
        many.push_back(VirtualRow{.id = index, .name = "row-" + std::to_string(index)});
    }
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

// Mutation: draw the selection fill under a select column too (the selected row is filled), or leave the Basic
// style's indicator on the boxes (their unchecked border is not the control border).
TEST_CASE("qt_quick table: with a select column a selected row has the bar and no fill, and the boxes are themed",
          "[qt_quick]") {
    Scene scene;
    Rows table = selectTable(scene);
    table.table->setSelection({ui::Key{std::int64_t{2}}});
    scene.settle();
    auto* const theme = scene.engine().singletonInstance<QObject*>(QStringLiteral("MorphUi"), QStringLiteral("Theme"));
    REQUIRE(theme != nullptr);
    CHECK(partOf(*table.rows.at(1), "selectionBar").isVisible());
    CHECK(partOf(*table.rows.at(1), "fill").property("color").value<QColor>().alpha() == 0);
    for (QQuickItem* const box : {&selectBoxOf(*table.rows.at(0)), &selectAllOf(*table.table)}) {
        auto const* const indicator = box->property("indicator").value<QQuickItem*>();
        REQUIRE(indicator != nullptr);
        CHECK(indicator->objectName() == QStringLiteral("boxIndicator"));
        CHECK(indicator->property("scheme").isValid());
    }
    // An unchecked row's box is outlined in the control border; the checked one is filled.
    auto const* const unchecked = selectBoxOf(*table.rows.at(0)).property("indicator").value<QQuickItem*>();
    CHECK(unchecked->property("border").value<QObject*>()->property("color").value<QColor>() ==
          theme->property("controlBorder").value<QColor>());
    auto const* const checked = selectBoxOf(*table.rows.at(1)).property("indicator").value<QQuickItem*>();
    CHECK(checked->property("color").value<QColor>() == theme->property("accent").value<QColor>());
    // The select-all box shows a partial selection as a dash.
    auto const* const all = selectAllOf(*table.table).property("indicator").value<QQuickItem*>();
    CHECK(all->property("partial").toBool());
    CHECK(all->findChild<QQuickItem*>(QStringLiteral("mark"))->property("text").toString() == QStringLiteral("–"));
}

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
    QAccessibleInterface const* const rowBox = QAccessible::queryAccessibleInterface(&selectBoxOf(*table.rows.at(1)));
    QAccessibleInterface const* const allBox = QAccessible::queryAccessibleInterface(&selectAll);
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
            {{.label = "Sample"}}, [&samples] { return std::vector<Sample>{samples}; },
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

namespace {

// A header label of a table, by its column: a Repeater's delegate, so found through the item tree.
QQuickItem& headerOf(ui::Widget const& table, int column) {
    QString const name = QStringLiteral("header%1").arg(column);
    QList<QQuickItem*> pending{&Scene::itemOf(table)};
    while (!pending.isEmpty()) {
        QQuickItem* const item = pending.takeLast();
        if (item->objectName() == name) {
            return *item;
        }
        for (QQuickItem* const child : item->childItems()) {
            pending.append(child);
        }
    }
    FAIL("no header " << column);
    throw std::logic_error{"unreachable"};
}

}  // namespace

// Mutation: leave the label's text without the mark (the sorted header reads `Name`).
TEST_CASE("qt_quick table: the sorted column's header shows and states its direction, and a click asks to sort",
          "[qt_quick]") {
    Scene scene;
    Rows table = makeTable(scene, {ui::Key{std::int64_t{1}}}, ui::SelectionMode::None);
    table.table->setColumns({{.label = "Name", .width = ui::Sizing::fixed(12), .sortable = true}, {.label = "Mass"}});
    std::vector<std::size_t> asked;
    table.table->setOnSort([&asked](std::size_t column) { asked.push_back(column); });
    scene.settle();
    QQuickItem& name = headerOf(*table.table, 0);
    QQuickItem const& mass = headerOf(*table.table, 1);
    CHECK(name.property("text").toString() == QStringLiteral("Name"));
    CHECK(name.property("activeFocusOnTab").toBool());
    CHECK_FALSE(mass.property("activeFocusOnTab").toBool());

    table.table->setSort(ui::TableSort{.column = 0, .direction = ui::SortDirection::Descending});
    scene.settle();
    CHECK(name.property("text").toString() == QStringLiteral("Name ▼"));
    CHECK(asked.empty());
    bool const wasActive = QAccessible::isActive();
    QAccessible::setActive(true);
    QAccessibleInterface const* const header = QAccessible::queryAccessibleInterface(&name);
    REQUIRE(header != nullptr);
    CHECK(header->text(QAccessible::Description) == QStringLiteral("Sorted descending"));
    CHECK(header->text(QAccessible::Name) == QStringLiteral("Name"));
    QAccessible::setActive(wasActive);

    scene.click(name);
    scene.click(mass);
    CHECK(asked == std::vector<std::size_t>{0});
    table.table->setSort(std::nullopt);
    scene.settle();
    CHECK(name.property("text").toString() == QStringLiteral("Name"));
}

// Control: ignore the token in MorphTableRow's `place` (the cell's top is the row's top, 0).
TEST_CASE("qt_quick table: a cell is inset by table.cellPaddingY and a row grows by twice it", "[qt_quick]") {
    Scene scene;
    Rows table = makeTable(scene, {ui::Key{std::string{"a"}}}, ui::SelectionMode::Single);
    QQuickItem const& rowItem = Scene::itemOf(*table.rows.at(0));
    QQuickItem const& cell = Scene::itemOf(*table.cells.at(0));
    qreal const restingTop = cell.mapToItem(&rowItem, QPointF{0, 0}).y();
    qreal const restingHeight = rowItem.height();
    CHECK(restingTop >= 4.0);

    ui::ThemeValues values = ui::themeValues();
    values.dimensions.insert_or_assign("table.cellPaddingY", 6.0);
    values.dimensions.insert_or_assign("table.cellPaddingX", 12.0);
    REQUIRE(morph::qt_quick::applyTheme(scene.engine(), values).empty());
    scene.settle();
    CHECK(cell.mapToItem(&rowItem, QPointF{0, 0}).y() >= 6.0);
    CHECK(cell.mapToItem(&rowItem, QPointF{0, 0}).x() == 12.0);
    CHECK(rowItem.height() >= restingHeight);
    CHECK(rowItem.height() >= cell.implicitHeight() + 12.0);
}

// Mutation: today's `figuresOf`, which rebuilds the font without its capitalisation and letter spacing.
TEST_CASE("qt_quick table: an overline cell keeps its capitals and letter spacing beside tabular figures",
          "[qt_quick]") {
    Scene scene;
    ui::ThemeValues values = ui::themeValues();
    values.keywords.insert_or_assign("font.overline.case", "uppercase");
    REQUIRE(morph::qt_quick::applyTheme(scene.engine(), values).empty());
    auto table = scene.backend().createTable(nullptr);
    table->setColumns(
        {{.label = "Kind", .width = ui::Sizing::fixed(12), .role = ui::TextRole::Overline, .align = ui::Align::End},
         {.label = "Name"}});
    auto row = scene.backend().createStack(table.get(), ui::Axis::Horizontal);
    table->setRowKey(*row, ui::Key{std::int64_t{1}});
    auto kind = scene.backend().createText(row.get());
    kind->setRole(ui::TextRole::Overline);
    kind->setText("Assay 7");
    auto name = scene.backend().createText(row.get());
    name->setText("pH");
    scene.settle();
    QFont const font = Scene::controlOf(*kind).property("font").value<QFont>();
    CHECK(font.featureValue(QFont::Tag{"tnum"}) == 1);
    CHECK(font.capitalization() == QFont::AllUppercase);
    CHECK(font.letterSpacing() > 0.0);
}

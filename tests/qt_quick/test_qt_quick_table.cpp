// SPDX-License-Identifier: Apache-2.0
//
// The table on the real Qt Quick engine: cells placed by column, selection by key in each mode, activation without
// toggling, and keys that cross into QML as text, an int64 above 2^53 unchanged.

#include <QAccessible>
#include <QAccessibleInterface>
#include <QColor>
#include <QFont>
#include <QFontMetricsF>
#include <QList>
#include <QObject>
#include <QPointF>
#include <QPointer>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlExpression>
#include <QQuickItem>
#include <QSizeF>
#include <QStringList>
#include <QTest>
#include <QVariantList>
#include <QtGlobal>
#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <morph/forms/engine/form_session.hpp>
#include <morph/qt_quick/theme.hpp>
#include <morph/reactive/signal.hpp>
#include <morph/ui/backend.hpp>
#include <morph/ui/grid.hpp>
#include <morph/ui/locale_table.hpp>
#include <morph/ui/mount.hpp>
#include <morph/ui/theme.hpp>
#include <morph/ui/view.hpp>
#include <optional>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "grid_test_support.hpp"
#include "qt_quick/kinds.hpp"
#include "qt_quick_harness.hpp"
#include "ui_fake_rows.hpp"

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

// The first item at or under @p root, by visual children, named @p name; null for none. A Repeater's delegates are not
// QObject children of the item they sit in, so `findChild` does not reach them.
// NOLINTNEXTLINE(misc-no-recursion): the items form a tree, and the search descends it.
QQuickItem* itemNamed(QQuickItem& root, QString const& name) {
    if (root.objectName() == name) {
        return &root;
    }
    for (QQuickItem* const child : root.childItems()) {
        if (QQuickItem* const found = itemNamed(*child, name)) {
            return found;
        }
    }
    return nullptr;
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

// Mutation: leave `elideToWidth` unset in MorphTableRow.place (the label runs past the badge into the next column).
TEST_CASE("qt_quick table: a badge cell narrower than its label elides it and stays inside its column",
          "[qt_quick][badge]") {
    Scene scene;
    auto table = scene.backend().createTable(nullptr);
    table->setLayout({.width = ui::Sizing::fixed(40)});
    table->setColumns({{.label = "Analyte", .width = ui::Sizing::fixed(8)},
                       {.label = "Evaluation", .width = ui::Sizing::stretch(1), .shrink = true},
                       {.label = "Change", .width = ui::Sizing::fixed(8)}});
    auto row = scene.backend().createStack(table.get(), ui::Axis::Horizontal);
    table->setRowKey(*row, ui::Key{std::int64_t{1}});
    auto analyte = scene.backend().createText(row.get());
    analyte->setText("Impurity A");
    auto badge = scene.backend().createBadge(row.get());
    badge->setText("Out of trend: \u22120.7 % vs. last 5 lots");
    auto change = scene.backend().createText(row.get());
    change->setText("0.12");
    scene.settle();
    QQuickItem const& badgeItem = Scene::itemOf(*badge);
    QQuickItem const& changeItem = Scene::itemOf(*change);
    auto* const label = badgeItem.findChild<QQuickItem*>(QStringLiteral("label"));
    REQUIRE(label != nullptr);
    CHECK(badgeItem.x() + badgeItem.width() <= changeItem.x() + 0.5);
    CHECK(label->property("truncated").toBool());
    CHECK(label->width() <= badgeItem.width());
    CHECK(badgeItem.property("tooltipText").toString() == QStringLiteral("Out of trend: \u22120.7 % vs. last 5 lots"));
    CHECK(badgeItem.property("text").toString() == QStringLiteral("Out of trend: \u22120.7 % vs. last 5 lots"));
}

// Mutation: share the spare width as the quotients alone (at some widths the shares sum a hair over or under the width
// and the sweep fails the exact-sum check), or compare `columnsWidth > width` (the hair makes the table interactive).
TEST_CASE("qt_quick table: stretch columns sum to the width that is left, and a table that fits does not scroll",
          "[qt_quick][table]") {
    Scene scene;
    auto table = scene.backend().createTable(nullptr);
    table->setLayout({.width = ui::Sizing::stretch(), .height = ui::Sizing::stretch()});
    // Weights that do not divide the width evenly, as the review screen's worksheet table has.
    table->setColumns({{.label = "A", .width = ui::Sizing::stretch(1)},
                       {.label = "B", .width = ui::Sizing::stretch(3)},
                       {.label = "C", .width = ui::Sizing::stretch(1)},
                       {.label = "D", .width = ui::Sizing::stretch(1)}});
    auto row = scene.backend().createStack(table.get(), ui::Axis::Horizontal);
    table->setRowKey(*row, ui::Key{std::int64_t{1}});
    std::vector<std::unique_ptr<ui::TextWidget>> cells;
    for (char const* text : {"a", "b", "c", "d"}) {
        cells.push_back(scene.backend().createText(row.get()));
        cells.back()->setText(text);
    }
    QQuickItem const& tableItem = Scene::itemOf(*table);
    int unequal = 0;
    int interactive = 0;
    for (int width = 300; width <= 900; ++width) {
        scene.window().resize(width, 400);
        scene.settle();
        qreal const sum = tableItem.property("columnsWidth").toReal();
        unequal += sum == tableItem.width() ? 0 : 1;
        auto* const flick = tableItem.findChild<QQuickItem*>(QStringLiteral("hflick"));
        REQUIRE(flick != nullptr);
        interactive += flick->property("interactive").toBool() ? 1 : 0;
    }
    CHECK(unequal == 0);
    CHECK(interactive == 0);
}

// Mutation: make `interactive` true for any excess (the table that is a hair wider than the window scrolls).
TEST_CASE("qt_quick table: a table scrolls sideways only when its columns are 1 px or more wider than it",
          "[qt_quick][table]") {
    Scene scene;
    auto table = scene.backend().createTable(nullptr);
    table->setLayout({.width = ui::Sizing::stretch(), .height = ui::Sizing::stretch()});
    table->setColumns(
        {{.label = "A", .width = ui::Sizing::fixed(20)}, {.label = "B", .width = ui::Sizing::fixed(20)}});
    auto row = scene.backend().createStack(table.get(), ui::Axis::Horizontal);
    table->setRowKey(*row, ui::Key{std::int64_t{1}});
    auto first = scene.backend().createText(row.get());
    first->setText("a");
    auto second = scene.backend().createText(row.get());
    second->setText("b");
    scene.settle();
    QQuickItem const& tableItem = Scene::itemOf(*table);
    auto* const flick = tableItem.findChild<QQuickItem*>(QStringLiteral("hflick"));
    REQUIRE(flick != nullptr);
    qreal const needed = tableItem.property("columnsWidth").toReal();
    scene.window().resize(static_cast<int>(std::ceil(needed)), 400);
    scene.settle();
    CHECK_FALSE(flick->property("interactive").toBool());
    scene.window().resize(static_cast<int>(std::floor(needed)) - 1, 400);
    scene.settle();
    CHECK(flick->property("interactive").toBool());
}

// Mutation: write the column's alignment over a text that has its own (the start-aligned text becomes right-aligned).
TEST_CASE("qt_quick table: a text with its own alignment keeps it in an end column", "[qt_quick][align]") {
    Scene scene;
    auto table = scene.backend().createTable(nullptr);
    table->setColumns({{.label = "Name", .width = ui::Sizing::fixed(12)},
                       {.label = "Area", .width = ui::Sizing::fixed(12), .align = ui::Align::End}});
    auto row = scene.backend().createStack(table.get(), ui::Axis::Horizontal);
    table->setRowKey(*row, ui::Key{std::int64_t{1}});
    auto name = scene.backend().createText(row.get());
    name->setText("pH");
    auto own = scene.backend().createText(row.get());
    own->setText("0.21");
    own->setAlign(ui::TextAlign::Start);
    scene.settle();
    CHECK(Scene::itemOf(*own).property("textAlign").toInt() == static_cast<int>(Qt::AlignLeft));
    // The same cell without an alignment of its own takes its column's.
    auto second = scene.backend().createStack(table.get(), ui::Axis::Horizontal);
    table->setRowKey(*second, ui::Key{std::int64_t{2}});
    auto secondName = scene.backend().createText(second.get());
    secondName->setText("pH");
    auto secondArea = scene.backend().createText(second.get());
    secondArea->setText("0.21");
    scene.settle();
    CHECK(Scene::itemOf(*secondArea).property("textAlign").toInt() == static_cast<int>(Qt::AlignRight));
}

// Mutation: leave a text with spans out of `oneLine` (the runs wrap onto two lines whatever the column says), or leave
// `fittedRuns` as the runs (the single line runs past the column's edge).
TEST_CASE("qt_quick table: spans in a column without wrap keep one line and end in an ellipsis", "[qt_quick]") {
    Scene scene;
    auto table = scene.backend().createTable(nullptr);
    table->setColumns({{.label = "Narrow", .width = ui::Sizing::fixed(5), .wrap = false},
                       {.label = "Roomy", .width = ui::Sizing::fixed(30), .wrap = false},
                       {.label = "Wrapping", .width = ui::Sizing::fixed(5)}});
    auto row = scene.backend().createStack(table.get(), ui::Axis::Horizontal);
    table->setRowKey(*row, ui::Key{std::int64_t{1}});
    std::vector<ui::TextSpan> const spans{{.text = "99.4"}, {.text = " % LC", .strong = true}};
    auto narrow = scene.backend().createText(row.get());
    narrow->setSpans(spans);
    auto roomy = scene.backend().createText(row.get());
    roomy->setSpans(spans);
    auto wrapping = scene.backend().createText(row.get());
    wrapping->setSpans(spans);
    scene.settle();
    QQuickItem const& narrowItem = Scene::itemOf(*narrow);
    CHECK(Scene::controlOf(*narrow).property("lineCount").toInt() == 1);
    CHECK(narrowItem.property("elided").toBool());
    CHECK(narrowItem.property("tooltipText").toString() == QStringLiteral("99.4 % LC"));
    CHECK(Scene::controlOf(*narrow).property("contentWidth").toDouble() <= narrowItem.width() + 1);
    CHECK(Scene::controlOf(*roomy).property("lineCount").toInt() == 1);
    CHECK_FALSE(Scene::itemOf(*roomy).property("elided").toBool());
    CHECK(Scene::controlOf(*wrapping).property("lineCount").toInt() > 1);
    CHECK_FALSE(Scene::itemOf(*wrapping).property("elided").toBool());
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
          "[qt_quick][heavy]") {
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
TEST_CASE("qt_quick table: a single-row grow's scan cost does not scale with row count", "[qt_quick][heavy]") {
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
    auto const surface = theme->property("surface").value<QColor>();
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

// Mutation: MorphTable.qml ignoring `busy` (MorphTableRow's busy mark never shown), under which row 1 shows no busy
// mark.
TEST_CASE("qt_quick table: a row's marks are drawn", "[qt_quick]") {
    Scene scene;
    Rows table = makeTable(scene, {ui::Key{std::string{"a"}}, ui::Key{std::string{"b"}}, ui::Key{std::string{"c"}}},
                           ui::SelectionMode::Single);
    table.table->setRowMarks(*table.rows.at(0),
                             ui::RowMarks{.unmatched = true, .tooltip = "This row no longer matches the filter"});
    table.table->setRowMarks(*table.rows.at(1), ui::RowMarks{.busy = true});
    table.table->setRowMarks(*table.rows.at(2),
                             ui::RowMarks{.locked = true, .tooltip = "Being edited by another user"});
    scene.settle();
    auto* const theme = scene.engine().singletonInstance<QObject*>(QStringLiteral("MorphUi"), QStringLiteral("Theme"));
    REQUIRE(theme != nullptr);
    auto const markOf = [](ui::StackWidget const& row, char const* name) -> QQuickItem& {
        auto* const found = Scene::itemOf(row).findChild<QQuickItem*>(QString::fromLatin1(name));
        REQUIRE(found != nullptr);
        return *found;
    };
    QQuickItem const& muted = Scene::itemOf(*table.rows.at(0));
    CHECK(muted.opacity() == theme->property("mutedOpacity").toReal());
    CHECK(muted.property("tooltipText").toString() == QStringLiteral("This row no longer matches the filter"));
    CHECK(markOf(*table.rows.at(1), "busyMark").isVisible());
    CHECK_FALSE(markOf(*table.rows.at(0), "busyMark").isVisible());
    CHECK(markOf(*table.rows.at(2), "lockedWash").isVisible());
    // The marks draw in the theme's tokens, and the busy bar sits inside the selection's.
    CHECK(markOf(*table.rows.at(1), "busyMark").property("color").value<QColor>() ==
          theme->property("tableBusy").value<QColor>());
    CHECK(markOf(*table.rows.at(1), "busyMark").x() == 3.0);
    CHECK(markOf(*table.rows.at(2), "lockedWash").property("color").value<QColor>() ==
          theme->property("tableLocked").value<QColor>());
    CHECK(Scene::itemOf(*table.rows.at(1)).property("tooltipText").toString() == QStringLiteral("Saving this row"));
    CHECK(Scene::itemOf(*table.rows.at(2)).property("tooltipText").toString() ==
          QStringLiteral("Being edited by another user"));
    for (auto const& row : table.rows) {
        table.table->setRowMarks(*row, ui::RowMarks{});
    }
    scene.settle();
    for (auto const& row : table.rows) {
        CHECK(Scene::itemOf(*row).opacity() == 1.0);
        CHECK_FALSE(markOf(*row, "busyMark").isVisible());
        CHECK_FALSE(markOf(*row, "lockedWash").isVisible());
        CHECK(Scene::itemOf(*row).property("tooltipText").toString().isEmpty());
    }
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
        rows.reserve(20);
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

namespace {

// A table of two fixed columns of 20 units and a stretch column under a long header, 60 units wide, with one row.
struct ShrinkTable {
    ShrinkTable(Scene& scene, bool shrink) {
        table = scene.backend().createTable(nullptr);
        table->setLayout({.width = ui::Sizing::fixed(60), .height = {}});
        table->setColumns(
            {{.label = "Sample", .width = ui::Sizing::fixed(20)},
             {.label = "Lot", .width = ui::Sizing::fixed(20)},
             {.label = "A rather long analyte heading", .width = ui::Sizing::stretch(1), .shrink = shrink}});
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

// Mutation: leave `shrink` out of the column's floor in MorphTable.qml (the stretch column keeps its header's width
// and the table scrolls), or let a column without `shrink` go under its header.
TEST_CASE("qt_quick table: a stretch column that shrinks goes under its header, one that does not scrolls the table",
          "[qt_quick]") {
    Scene scene;
    ShrinkTable const shrinking{scene, true};
    QQuickItem const& item = Scene::itemOf(*shrinking.table);
    QVariantList const widths = item.property("widths").toList();
    REQUIRE(widths.size() == 3);
    qreal const unit = units(scene).width();
    CHECK(widths.at(2).toReal() >= (8 * unit) - 0.5);
    auto const* const flick = item.findChild<QQuickItem*>(QStringLiteral("hflick"));
    REQUIRE(flick != nullptr);
    CHECK_FALSE(flick->property("interactive").toBool());

    Scene other;
    ShrinkTable const keeping{other, false};
    QQuickItem const& keepingItem = Scene::itemOf(*keeping.table);
    QVariantList const kept = keepingItem.property("widths").toList();
    REQUIRE(kept.size() == 3);
    CHECK(kept.at(2).toReal() > widths.at(2).toReal() + unit);
    auto const* const keepingFlick = keepingItem.findChild<QQuickItem*>(QStringLiteral("hflick"));
    REQUIRE(keepingFlick != nullptr);
    CHECK(keepingFlick->property("interactive").toBool());
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
    for (QQuickItem const* const box : {&selectBoxOf(*table.rows.at(0)), &selectAllOf(*table.table)}) {
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

// Mutation: leave the label's text without the mark (the sorted header reads `Name`); read the Shift state from the
// tapped signal's event point, which has no modifiers (every click reports `Plain`).
TEST_CASE("qt_quick table: the sorted column's header shows and states its direction, and a click asks to sort",
          "[qt_quick]") {
    Scene scene;
    Rows table = makeTable(scene, {ui::Key{std::int64_t{1}}}, ui::SelectionMode::None);
    table.table->setColumns({{.label = "Name", .width = ui::Sizing::fixed(12), .sortable = true}, {.label = "Mass"}});
    std::vector<std::size_t> asked;
    std::vector<ui::HeaderClick> clicks;
    table.table->setOnSort([&asked, &clicks](std::size_t column, ui::HeaderClick click) {
        asked.push_back(column);
        clicks.push_back(click);
    });
    scene.settle();
    QQuickItem& name = headerOf(*table.table, 0);
    QQuickItem const& mass = headerOf(*table.table, 1);
    CHECK(name.property("text").toString() == QStringLiteral("Name"));
    CHECK(name.property("activeFocusOnTab").toBool());
    CHECK_FALSE(mass.property("activeFocusOnTab").toBool());

    table.table->setSort({ui::TableSort{.column = 0, .direction = ui::SortDirection::Descending}});
    scene.settle();
    CHECK(name.property("text").toString() == QStringLiteral("Name ▼"));
    CHECK(asked.empty());
    bool const wasActive = QAccessible::isActive();
    QAccessible::setActive(true);
    QAccessibleInterface const* const header = QAccessible::queryAccessibleInterface(&name);
    REQUIRE(header != nullptr);
    CHECK(header->text(QAccessible::Description) == QStringLiteral("sorted descending"));
    CHECK(header->text(QAccessible::Name) == QStringLiteral("Name"));
    QAccessible::setActive(wasActive);

    scene.click(name);
    scene.click(mass);
    CHECK(asked == std::vector<std::size_t>{0});
    // Shift held reports `Shift`; Ctrl, which the contract gives no meaning on a header, reports `Plain`.
    scene.click(name, Qt::ShiftModifier);
    scene.click(name, Qt::ControlModifier);
    CHECK(clicks ==
          std::vector<ui::HeaderClick>{ui::HeaderClick::Plain, ui::HeaderClick::Shift, ui::HeaderClick::Plain});
    table.table->setSort({});
    scene.settle();
    CHECK(name.property("text").toString() == QStringLiteral("Name"));
}

// Controls: report the edge drag's width in pixels instead of units; read the drag's translation once it has ended
// (no change of width); leave the filter button above the edge (the filterable header's drag reaches no edge).
TEST_CASE("qt_quick table: dragging a header's trailing edge reports units, a double click on it none",
          "[qt_quick][table]") {
    bool const filterable = GENERATE(false, true);
    CAPTURE(filterable);
    Scene scene;
    Rows table = makeTable(scene, {ui::Key{std::int64_t{1}}}, ui::SelectionMode::None);
    table.table->setColumns(
        {{.label = "Name", .width = ui::Sizing::fixed(12), .filterable = filterable}, {.label = "Mass"}});
    int filters = 0;
    table.table->setOnFilter([&filters](std::size_t /*column*/) { ++filters; });
    std::vector<std::pair<std::size_t, std::optional<int>>> resized;
    table.table->setOnColumnResize(
        [&resized](std::size_t column, std::optional<int> units) { resized.emplace_back(column, units); });
    scene.settle();
    QQuickItem const& name = headerOf(*table.table, 0);
    QQuickItem* edge = nullptr;
    for (QQuickItem* const child : name.childItems()) {
        if (child->objectName() == QStringLiteral("edge0")) {
            edge = child;
        }
    }
    REQUIRE(edge != nullptr);
    // The header is the column's 12 units and its padding; four units more to the right make 16 units of content.
    qreal const unit = units(scene).width();
    QPoint const start = Scene::centreOf(*edge);
    QPoint const end = start + QPoint{static_cast<int>(std::lround(4 * unit)), 0};
    QTest::mousePress(&scene.window(), Qt::LeftButton, {}, start);
    constexpr int steps = 10;
    for (int step = 1; step <= steps; ++step) {
        QTest::mouseMove(&scene.window(), start + ((end - start) * step / steps));
    }
    QTest::mouseRelease(&scene.window(), Qt::LeftButton, {}, end);
    scene.settle();
    REQUIRE(resized.size() == 1);
    CHECK(resized.front().first == 0);
    CHECK(resized.front().second == std::optional<int>{16});
    scene.doubleClick(*edge);
    REQUIRE_FALSE(resized.empty());
    CHECK(resized.back() == std::pair<std::size_t, std::optional<int>>{0, std::nullopt});
    CHECK(filters == 0);
}

// Control: ignore a drag that ends on another header.
TEST_CASE("qt_quick table: dragging a header onto another reports the move", "[qt_quick][table]") {
    Scene scene;
    Rows table = makeTable(scene, {ui::Key{std::int64_t{1}}}, ui::SelectionMode::None);
    table.table->setColumns({{.label = "Name", .width = ui::Sizing::fixed(10)},
                             {.label = "Mass", .width = ui::Sizing::fixed(10)},
                             {.label = "Site", .width = ui::Sizing::fixed(10)}});
    std::vector<std::pair<std::size_t, std::size_t>> moves;
    table.table->setOnColumnMove(
        [&moves](std::size_t column, std::size_t position) { moves.emplace_back(column, position); });
    scene.settle();
    QPoint const start = Scene::centreOf(headerOf(*table.table, 0));
    QPoint const end = Scene::centreOf(headerOf(*table.table, 2));
    QTest::mousePress(&scene.window(), Qt::LeftButton, {}, start);
    constexpr int steps = 10;
    for (int step = 1; step <= steps; ++step) {
        QTest::mouseMove(&scene.window(), start + ((end - start) * step / steps));
    }
    QTest::mouseRelease(&scene.window(), Qt::LeftButton, {}, end);
    scene.settle();
    CHECK(moves == std::vector<std::pair<std::size_t, std::size_t>>{{0, 2}});
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
    auto const font = Scene::controlOf(*kind).property("font").value<QFont>();
    CHECK(font.featureValue(QFont::Tag{"tnum"}) == 1);
    CHECK(font.capitalization() == QFont::AllUppercase);
    CHECK(font.letterSpacing() > 0.0);
}

namespace {

/// The descendant item of @p item whose objectName is @p name, or null.
QQuickItem* descendantNamed(QQuickItem const& item, QString const& name) {
    QList<QQuickItem const*> pending{&item};
    while (!pending.isEmpty()) {
        QQuickItem const* const node = pending.takeLast();
        for (QQuickItem* const child : node->childItems()) {
            if (child->objectName() == name) {
                return child;
            }
            pending.append(child);
        }
    }
    return nullptr;
}

}  // namespace

// Mutation: draw the header in `Theme.smallStrongFont` whatever the token (no capitals), or size the columns from it
// (the column is narrower than its capitals).
TEST_CASE("qt_quick table: a column header follows field.caption.case and its column fits the capitals",
          "[qt_quick]") {
    Scene scene;
    auto table = scene.backend().createTable(nullptr);
    table->setColumns({{.label = "Sample"}, {.label = "Mixing plant and sampling point"}});
    REQUIRE(morph::qt_quick::applyTheme(scene.engine(), ui::themeValues()).empty());
    scene.settle();
    QQuickItem const& tableItem = Scene::itemOf(*table);
    auto const headerOf = [&](char const* name) {
        auto* const header = descendantNamed(tableItem, QString::fromLatin1(name));
        REQUIRE(header != nullptr);
        return header;
    };
    auto const plain = headerOf("header0")->property("font").value<QFont>();
    CHECK(plain.capitalization() == QFont::MixedCase);
    qreal const plainWidth = tableItem.property("widths").toList().at(1).toReal();

    ui::ThemeValues values = ui::themeValues();
    values.keywords.insert_or_assign("field.caption.case", "uppercase");
    REQUIRE(morph::qt_quick::applyTheme(scene.engine(), values).empty());
    scene.settle();
    for (char const* const name : {"header0", "header1"}) {
        auto const font = headerOf(name)->property("font").value<QFont>();
        CHECK(font.capitalization() == QFont::AllUppercase);
        CHECK(font.letterSpacing() > 0.0);
    }
    auto const font = headerOf("header1")->property("font").value<QFont>();
    qreal const capitals = QFontMetricsF{font}.horizontalAdvance(QStringLiteral("MIXING PLANT AND SAMPLING POINT"));
    CHECK(tableItem.property("widths").toList().at(1).toReal() >= capitals + columnPadding(scene));
    CHECK(tableItem.property("widths").toList().at(1).toReal() > plainWidth);
    // The header's text and accessible name stay as written.
    CHECK(headerOf("header1")->property("text").toString() == QStringLiteral("Mixing plant and sampling point"));
    bool const wasActive = QAccessible::isActive();
    QAccessible::setActive(true);
    QAccessibleInterface const* const node = QAccessible::queryAccessibleInterface(headerOf("header1"));
    REQUIRE(node != nullptr);
    CHECK(node->text(QAccessible::Name) == QStringLiteral("Mixing plant and sampling point"));
    QAccessible::setActive(wasActive);
}

namespace {

// The warnings of the `morph.layout` category, collected while a `TableWatch` lives.
QStringList& tableWarnings() {
    static QStringList warnings;
    return warnings;
}

QtMessageHandler& previousTableHandler() {
    static QtMessageHandler previous = nullptr;
    return previous;
}

void collectTableWarnings(QtMsgType type, QMessageLogContext const& context, QString const& message) {
    if (type == QtWarningMsg && context.category != nullptr && std::string_view{context.category} == "morph.layout") {
        tableWarnings().append(message);
    } else if (previousTableHandler() != nullptr) {
        previousTableHandler()(type, context, message);
    }
}

class TableWatch {
public:
    TableWatch() : _previous{qInstallMessageHandler(collectTableWarnings)} {
        previousTableHandler() = _previous;
        tableWarnings().clear();
    }
    ~TableWatch() { qInstallMessageHandler(_previous); }
    TableWatch(TableWatch const&) = delete;
    TableWatch& operator=(TableWatch const&) = delete;
    TableWatch(TableWatch&&) = delete;
    TableWatch& operator=(TableWatch&&) = delete;

private:
    QtMessageHandler _previous;
};

}  // namespace

// Mutation: leave the bar to the Basic style's own (hidden at rest), draw it over the last row, or report nothing.
TEST_CASE("qt_quick table: a table that scrolls sideways shows its bar at rest, above the rows, and says so once",
          "[qt_quick]") {
    TableWatch const watch;
    Scene scene;
    WideTable const wide{scene, 60};
    wide.table->setTestId("conditions");
    scene.settle();
    QTest::qWait(150);
    QQuickItem const& tableItem = Scene::itemOf(*wide.table);
    auto const* const hflick = tableItem.findChild<QQuickItem*>(QStringLiteral("hflick"));
    REQUIRE(hflick != nullptr);
    REQUIRE(hflick->property("interactive").toBool());
    auto const* const bar = qobject_cast<QQuickItem*>(tableItem.property("barItem").value<QObject*>());
    REQUIRE(bar != nullptr);
    CHECK(bar->isVisible());
    // The scrollBar part's instance: a restyle replaces it, and the plain bar carries the scroll area's scheme.
    CHECK(bar->property("scheme").isValid());
    CHECK(bar->property("policy").toInt() == 2);
    // The rows end above the bar.
    QQuickItem const& rowItem = Scene::itemOf(*wide.row);
    CHECK(rowItem.mapToItem(hflick, QPointF{0, rowItem.height()}).y() <= bar->y() + 0.5);
    REQUIRE(tableWarnings().size() == 1);
    CHECK(tableWarnings().front().contains(QStringLiteral("table")));
    CHECK(tableWarnings().front().contains(QStringLiteral("scrolls sideways: its columns need")));

    // A table that fits has no bar and says nothing.
    tableWarnings().clear();
    Scene roomy;
    roomy.window().resize(2000, 600);
    WideTable const fits{roomy, 200};
    auto const* const fitBar =
        qobject_cast<QQuickItem*>(Scene::itemOf(*fits.table).property("barItem").value<QObject*>());
    REQUIRE(fitBar != nullptr);
    CHECK_FALSE(fitBar->isVisible());
    QTest::qWait(150);
    CHECK(tableWarnings().isEmpty());
}

// Mutation: leave `honoursContentMinimum` on for a table row's cells (the cell is as wide as its content, and the row
// is out of line with the header).
TEST_CASE("qt_quick table: a cell whose content is wider than its column keeps its column", "[qt_quick]") {
    Scene scene;
    auto table = scene.backend().createTable(nullptr);
    table->setColumns(
        {{.label = "Sample", .width = ui::Sizing::fixed(10)}, {.label = "Lot", .width = ui::Sizing::fixed(10)}});
    auto row = scene.backend().createStack(table.get(), ui::Axis::Horizontal);
    table->setRowKey(*row, ui::Key{std::int64_t{1}});
    auto const packed = scene.backend().createStack(row.get(), ui::Axis::Horizontal);
    auto first = scene.backend().createText(packed.get());
    first->setText("a long sample identifier");
    auto second = scene.backend().createText(packed.get());
    second->setText("and a second long identifier");
    auto lot = scene.backend().createText(row.get());
    lot->setText("L-1");
    scene.settle();
    QQuickItem const& tableItem = Scene::itemOf(*table);
    QVariantList const widths = tableItem.property("widths").toList();
    REQUIRE(widths.size() == 2);
    QQuickItem const& packedItem = Scene::itemOf(*packed);
    QQuickItem const& lotItem = Scene::itemOf(*lot);
    CHECK(packedItem.width() <= widths.at(0).toReal() + 0.5);
    CHECK(lotItem.mapToItem(&tableItem, QPointF{0, 0}).x() - packedItem.mapToItem(&tableItem, QPointF{0, 0}).x() ==
          Catch::Approx(widths.at(0).toReal()).margin(1.0));
}

namespace {

// A table of stretch (and other) columns with one row of texts, made as the mount makes them.
struct Sheet {
    std::unique_ptr<ui::TableWidget> table;
    std::unique_ptr<ui::StackWidget> row;
    std::vector<std::unique_ptr<ui::TextWidget>> cells;
};

Sheet makeSheet(Scene& scene, std::vector<ui::TableColumn> const& columns, std::vector<std::string> const& texts) {
    Sheet sheet;
    sheet.table = scene.backend().createTable(nullptr);
    sheet.table->setLayout({.width = ui::Sizing::stretch(), .height = ui::Sizing::stretch()});
    sheet.table->setColumns(columns);
    sheet.row = scene.backend().createStack(sheet.table.get(), ui::Axis::Horizontal);
    sheet.table->setRowKey(*sheet.row, ui::Key{std::int64_t{1}});
    for (std::string const& text : texts) {
        sheet.cells.push_back(scene.backend().createText(sheet.row.get()));
        sheet.cells.back()->setText(text);
    }
    return sheet;
}

std::vector<qreal> widthsOf(QQuickItem const& table) {
    std::vector<qreal> out;
    for (QVariant const& width : table.property("widths").toList()) {
        out.push_back(width.toReal());
    }
    return out;
}

// The table's per-column {kind, natural, least, floor, weight}.
QVariantList measureOf(QQuickItem& table) {
    QVariant result;
    REQUIRE(QMetaObject::invokeMethod(&table, "measure", Qt::DirectConnection, Q_RETURN_ARG(QVariant, result)));
    return result.toList();
}

}  // namespace

// Mutation: weight the stretch columns without their content (the two columns are equal, 300 px each).
TEST_CASE("qt_quick table: a stretch column starts from its content, and the weights share only what is left",
          "[qt_quick][table]") {
    Scene scene;
    Sheet sheet = makeSheet(scene,
                            {{.label = "A", .width = ui::Sizing::stretch(1), .wrap = false},
                             {.label = "B", .width = ui::Sizing::stretch(1), .wrap = false}},
                            {"a", "a thirty-character text, whole"});
    scene.window().resize(600, 400);
    scene.settle();
    QQuickItem const& tableItem = Scene::itemOf(*sheet.table);
    auto const widths = widthsOf(tableItem);
    REQUIRE(widths.size() == 2);
    CHECK(widths.at(1) - widths.at(0) > 5 * units(scene).width());
    CHECK_FALSE(Scene::itemOf(*sheet.cells.at(1)).property("elided").toBool());
    CHECK(widths.at(0) + widths.at(1) == Catch::Approx(tableItem.width()));
}

// Mutation: skip step 2 (the wrapping column keeps its natural width and the one-line column gives, so it is cut), or
// share by weight alone (the one-line column is cut at the narrow widths).
TEST_CASE("qt_quick table: a wrapping column gives before a column that does not wrap", "[qt_quick][table]") {
    Scene scene;
    Sheet sheet = makeSheet(scene,
                            {{.label = "Note", .width = ui::Sizing::stretch(1)},
                             {.label = "Tag", .width = ui::Sizing::stretch(1), .wrap = false}},
                            {"alpha beta gamma delta epsilon zeta", "Out of specification"});
    scene.window().resize(300, 400);
    scene.settle();
    QQuickItem& tableItem = Scene::itemOf(*sheet.table);
    QVariantList const metrics = measureOf(tableItem);
    REQUIRE(metrics.size() == 2);
    auto const field = [&metrics](int column, char const* name) {
        return metrics.at(column).toMap().value(QString::fromLatin1(name)).toReal();
    };
    // The width where the wrapping column has given up all it can and the tag is still whole.
    qreal const room = field(0, "least") + field(1, "natural");
    REQUIRE(room < field(0, "natural") + field(1, "natural"));
    scene.window().resize(static_cast<int>(std::ceil(room)) + 2, 400);
    scene.settle();
    auto const widths = widthsOf(tableItem);
    REQUIRE(widths.size() == 2);
    CHECK(widths.at(1) >= field(1, "natural") - 1e-6);
    CHECK(widths.at(0) < field(0, "natural"));
    CHECK_FALSE(Scene::itemOf(*sheet.cells.at(1)).property("elided").toBool());
}

// Mutation: weights alone, or proportional shrinking, or step 2 skipped: the guard fails at the widths where its
// discriminating checks hold.
TEST_CASE("qt_quick table: no column narrows as the table widens, and the stretch columns sum to what is left",
          "[qt_quick][table]") {
    Scene scene;
    Sheet const sheet = makeSheet(scene,
                                  {{.label = "F", .width = ui::Sizing::fixed(6)},
                                   {.label = "C"},
                                   {.label = "W", .width = ui::Sizing::stretch(1)},
                                   {.label = "L", .width = ui::Sizing::stretch(2), .wrap = false},
                                   {.label = "S", .width = ui::Sizing::stretch(1), .wrap = false, .shrink = true},
                                   {.label = "O", .width = ui::Sizing::stretch(1), .wrap = false}},
                                  {"fixed", "content", "alpha beta gamma delta epsilon zeta eta theta",
                                   "one line of a few words", "another line here", "x"});
    QQuickItem& tableItem = Scene::itemOf(*sheet.table);
    auto const* const flick = tableItem.findChild<QQuickItem*>(QStringLiteral("hflick"));
    REQUIRE(flick != nullptr);
    std::vector<qreal> previous;
    int inFirst = 0;
    int inSecond = 0;
    int inThird = 0;
    for (int width = 200; width <= 1600; ++width) {
        scene.window().resize(width, 400);
        scene.settle();
        auto const widths = widthsOf(tableItem);
        QVariantList const metrics = measureOf(tableItem);
        REQUIRE(std::cmp_equal(widths.size(), metrics.size()));
        INFO("window width " << width);
        qreal used = 0;
        qreal natural = 0;
        qreal least = 0;
        qreal floor = 0;
        qreal stretched = 0;
        for (std::size_t i = 0; i < widths.size(); ++i) {
            QVariantMap const metric = metrics.at(static_cast<qsizetype>(i)).toMap();
            if (metric.value(QStringLiteral("kind")).toInt() == 2) {
                natural += metric.value(QStringLiteral("natural")).toReal();
                least += metric.value(QStringLiteral("least")).toReal();
                floor += metric.value(QStringLiteral("floor")).toReal();
                stretched += widths.at(i);
            } else {
                used += widths.at(i);
            }
            if (!previous.empty()) {
                CHECK(widths.at(i) >= previous.at(i) - 1e-6);
            }
        }
        qreal const left = std::max<qreal>(0, tableItem.width() - used);
        if (floor <= left) {
            CHECK(std::abs(stretched - left) < 1e-6);
            CHECK_FALSE(flick->property("interactive").toBool());
        }
        for (std::size_t i = 0; i < widths.size(); ++i) {
            QVariantMap const metric = metrics.at(static_cast<qsizetype>(i)).toMap();
            if (metric.value(QStringLiteral("kind")).toInt() != 2) {
                continue;
            }
            qreal const own = metric.value(QStringLiteral("natural")).toReal();
            if (left >= natural) {
                CHECK(widths.at(i) >= own - 1e-6);
            } else if (left >= least) {
                // Only the wrapping column (index 2) narrows; the one-line columns stay at their natural width.
                CHECK((i == 2 ? widths.at(i) < own : std::abs(widths.at(i) - own) < 1e-6));
            }
        }
        inFirst += left >= natural ? 1 : 0;
        inSecond += left < natural && left >= least ? 1 : 0;
        inThird += left < least && left >= floor ? 1 : 0;
        previous = widths;
    }
    // The sweep reaches every step, or its checks would hold for nothing.
    CHECK(inFirst > 100);
    CHECK(inSecond > 20);
    CHECK(inThird > 20);
}

namespace {

QStringList& loopMessages() {
    static QStringList messages;
    return messages;
}

QtMessageHandler& previousLoopHandler() {
    static QtMessageHandler previous = nullptr;
    return previous;
}

void collectLoopMessages(QtMsgType type, QMessageLogContext const& context, QString const& message) {
    if (type == QtWarningMsg && message.contains(QStringLiteral("Binding loop"))) {
        loopMessages().append(message);
    } else if (previousLoopHandler() != nullptr) {
        previousLoopHandler()(type, context, message);
    }
}

class LoopWatch {
public:
    LoopWatch() : _previous{qInstallMessageHandler(collectLoopMessages)} {
        previousLoopHandler() = _previous;
        loopMessages().clear();
    }
    ~LoopWatch() { qInstallMessageHandler(_previous); }
    LoopWatch(LoopWatch const&) = delete;
    LoopWatch& operator=(LoopWatch const&) = delete;
    LoopWatch(LoopWatch&&) = delete;
    LoopWatch& operator=(LoopWatch&&) = delete;

private:
    QtMessageHandler _previous;
};

// Lets queued calls (`Qt.callLater`) and bindings run until nothing changes.
void settleFully(Scene& scene, QQuickItem const& row) {
    int previous = -1;
    for (int turn = 0; turn < 50; ++turn) {
        scene.settle();
        QTest::qWait(5);
        int const passes = row.property("placePasses").toInt();
        if (passes == previous) {
            return;
        }
        previous = passes;
    }
}

}  // namespace

// Mutation: remove the `placing`/`placeAgain` guard from MorphTableRow.place (the nested call runs inside the outer
// one: no re-entry is held, and the outer frame resumes with positions it read before).
TEST_CASE("qt_quick table: a cell whose width follows its width does not leave a misaligned row",
          "[qt_quick][table]") {
    LoopWatch const watch;
    Scene scene;
    Sheet sheet = makeSheet(scene,
                            {{.label = "A", .width = ui::Sizing::stretch(1), .wrap = false},
                             {.label = "B", .width = ui::Sizing::stretch(1), .wrap = false},
                             {.label = "C", .width = ui::Sizing::stretch(1), .wrap = false},
                             {.label = "D", .width = ui::Sizing::stretch(1), .wrap = false}},
                            {"alpha", "beta", "gamma", "delta"});
    // The second cell asks for 20 more than the width it is given until that is 100, then 100: it changes as it is
    // placed, which reports to the table and places the row again while it is still placing.
    QQmlExpression follow{
        scene.engine().rootContext(), &Scene::itemOf(*sheet.cells.at(1)),
        QStringLiteral("implicitWidth = Qt.binding(function() { return width < 100 ? width + 20 : 100 })")};
    static_cast<void>(follow.evaluate());
    REQUIRE_FALSE(follow.hasError());
    scene.window().resize(700, 400);
    QQuickItem const& rowItem = Scene::itemOf(*sheet.row);
    settleFully(scene, rowItem);
    QQuickItem const& tableItem = Scene::itemOf(*sheet.table);
    auto const widths = widthsOf(tableItem);
    REQUIRE(widths.size() == 4);
    qreal sum = 0;
    for (std::size_t i = 0; i < sheet.cells.size(); ++i) {
        INFO("column " << i);
        // The header label of a column starts where the widths before it end; its cell sits an inset in.
        CHECK(Scene::itemOf(*sheet.cells.at(i)).x() == Catch::Approx(sum + kCellInset));
        sum += widths.at(i);
    }
    auto* const cells = rowItem.property("content").value<QObject*>();
    REQUIRE(cells != nullptr);
    CHECK(cells->property("implicitWidth").toReal() == Catch::Approx(sum));
    INFO(loopMessages().join(QLatin1Char('\n')).toStdString());
    CHECK(loopMessages().isEmpty());
    CHECK(rowItem.property("placePasses").toInt() > 0);
    // The follower did place the row again while it was placing, and the guard held that call for another pass.
    CHECK(rowItem.property("placeReentries").toInt() > 0);
}

// Mutation: read the cell's `implicitWidth` instead of its `widthFreeNatural` in `naturalOf` (the nested table keeps
// the width it was widened to as its natural width, and the siblings come out narrower than in a fresh build).
TEST_CASE("qt_quick table: a table in a cell settles to the layout a fresh build has", "[qt_quick][table]") {
    LoopWatch const watch;
    struct Nested {
        Sheet outer;
        std::unique_ptr<ui::TableWidget> inner;
        std::unique_ptr<ui::StackWidget> innerRow;
        std::vector<std::unique_ptr<ui::TextWidget>> innerCells;
    };
    auto const build = [](Scene& scene) {
        Nested made;
        made.outer.table = scene.backend().createTable(nullptr);
        made.outer.table->setLayout({.width = ui::Sizing::stretch(), .height = ui::Sizing::stretch()});
        made.outer.table->setColumns({{.label = "A", .width = ui::Sizing::stretch(1), .wrap = false},
                                      {.label = "Inner", .width = ui::Sizing::stretch(1), .wrap = false},
                                      {.label = "B", .width = ui::Sizing::stretch(1), .wrap = false}});
        made.outer.row = scene.backend().createStack(made.outer.table.get(), ui::Axis::Horizontal);
        made.outer.table->setRowKey(*made.outer.row, ui::Key{std::int64_t{1}});
        made.outer.cells.push_back(scene.backend().createText(made.outer.row.get()));
        made.outer.cells.back()->setText("sibling of some width");
        made.inner = scene.backend().createTable(made.outer.row.get());
        made.inner->setColumns({{.label = "X", .width = ui::Sizing::stretch(1), .wrap = false},
                                {.label = "Y", .width = ui::Sizing::stretch(1), .wrap = false}});
        made.innerRow = scene.backend().createStack(made.inner.get(), ui::Axis::Horizontal);
        made.inner->setRowKey(*made.innerRow, ui::Key{std::int64_t{1}});
        for (char const* text : {"inner x", "inner y of a longer text"}) {
            made.innerCells.push_back(scene.backend().createText(made.innerRow.get()));
            made.innerCells.back()->setText(text);
        }
        made.outer.cells.push_back(scene.backend().createText(made.outer.row.get()));
        made.outer.cells.back()->setText("another sibling of some width");
        return made;
    };
    // Built wide, then narrowed in steps, against the same table built narrow.
    Scene history;
    Nested const walked = build(history);
    QQuickItem const& walkedItem = Scene::itemOf(*walked.outer.table);
    history.window().resize(900, 400);
    settleFully(history, Scene::itemOf(*walked.outer.row));
    history.window().resize(1400, 400);
    settleFully(history, Scene::itemOf(*walked.outer.row));
    history.window().resize(700, 400);
    settleFully(history, Scene::itemOf(*walked.outer.row));

    Scene fresh;
    Nested const built = build(fresh);
    fresh.window().resize(700, 400);
    settleFully(fresh, Scene::itemOf(*built.outer.row));

    auto const a = widthsOf(walkedItem);
    auto const b = widthsOf(Scene::itemOf(*built.outer.table));
    REQUIRE(a.size() == 3);
    REQUIRE(b.size() == 3);
    for (std::size_t i = 0; i < 3; ++i) {
        INFO("column " << i);
        CHECK(a.at(i) == Catch::Approx(b.at(i)).margin(0.5));
    }
    CHECK(loopMessages().isEmpty());
}

// Mutation: read `label.implicitWidth` for runs that wrap (on Qt 6.8 the value follows the width it is given; on newer
// Qt the control may not fail, and only the computed value is asserted).
TEST_CASE("qt_quick table: the width-free natural width of runs that wrap is the widest line's advance",
          "[qt_quick][table]") {
    Scene scene;
    auto const column = scene.backend().createStack(nullptr, ui::Axis::Vertical);
    auto text = scene.backend().createText(column.get());
    text->setSpans({{.text = "first line\n"}, {.text = "the second line is longer", .strong = true}});
    scene.window().resize(400, 300);
    scene.settle();
    QQuickItem& item = Scene::itemOf(*text);
    auto const advanceOf = [&item](QString const& plain, bool strong) {
        QVariantMap run;
        run.insert(QStringLiteral("text"), plain);
        run.insert(QStringLiteral("strong"), strong);
        double advance = 0;
        REQUIRE(QMetaObject::invokeMethod(&item, "runsAdvance", Qt::DirectConnection, Q_RETURN_ARG(double, advance),
                                          Q_ARG(QVariant, QVariantList{run})));
        return advance;
    };
    qreal const slack = item.property("wrapSlack").toReal();
    qreal const expected = std::ceil(std::max(advanceOf(QStringLiteral("first line"), false),
                                              advanceOf(QStringLiteral("the second line is longer"), true)) +
                                     slack);
    item.setProperty("singleLine", false);
    for (int const width : {400, 120, 400}) {
        scene.window().resize(width, 300);
        scene.settle();
        INFO("window width " << width);
        CHECK(item.property("widthFreeNatural").toReal() == Catch::Approx(expected));
    }
}

// Mutation: let `rescanColumn` replace the caches when virtualized (the least and natural widths fall when the wide
// row dewindows).
TEST_CASE("qt_quick table: a virtualized table's stretch column caches only grow as a wide row dewindows",
          "[qt_quick][table]") {
    Scene scene;
    std::vector<VirtualRow> const many = [] {
        std::vector<VirtualRow> rows;
        rows.push_back(VirtualRow{.id = 0, .name = "a very very very very very very long row name indeed"});
        for (std::int64_t i = 1; i < 20; ++i) {
            rows.push_back(VirtualRow{.id = i, .name = "short"});
        }
        return rows;
    }();
    ui::Mounted const mounted{
        scene.runtime(), scene.backend(),
        ui::table<VirtualRow>(
            {{.label = "Name", .width = ui::Sizing::stretch(1)}}, [&many] { return std::vector<VirtualRow>{many}; },
            [](VirtualRow const& row) { return ui::Key{row.id}; }, virtualRowCell,
            {.virtualize = {.threshold = 5, .overscan = 1}, .common = {.layout = {.height = ui::Sizing::fixed(10)}}})};
    scene.settle();
    QQuickItem const& tableItem = Scene::itemOf(mounted.root());
    auto* const flick = tableItem.findChild<QQuickItem*>(QStringLiteral("flick"));
    REQUIRE(flick != nullptr);
    qreal const wide = tableItem.property("columnCache").toList().at(0).toReal();
    qreal const wideLeast = tableItem.property("leastCache").toList().at(0).toReal();
    flick->setProperty("contentY", flick->property("contentHeight").toReal() * 0.9);
    scene.settle();
    REQUIRE(tableItem.property("realizedFirst").toInt() > 0);
    CHECK(tableItem.property("columnCache").toList().at(0).toReal() == wide);
    CHECK(tableItem.property("leastCache").toList().at(0).toReal() == wideLeast);
}

namespace {

// The y of an item's top edge and bottom edge in the table's coordinates.
std::pair<qreal, qreal> verticalExtent(QQuickItem const& item, QQuickItem const& table) {
    QPointF const top = item.mapToItem(&table, QPointF{0, 0});
    return {top.y(), top.y() + item.height()};
}

}  // namespace

// Mutation: leave `setRowSpans` empty (the cell stays one row tall and the covered cells show), or skip the `covered`
// flag (the covered cells stay visible), or place the cell by the row's height (it ends with its row).
TEST_CASE("qt_quick table: a spanning cell reaches the foot of the rows it covers, and the covered cells are hidden",
          "[qt_quick][span]") {
    Scene scene;
    Rows table = makeTable(scene, {ui::Key{1}, ui::Key{2}, ui::Key{3}}, ui::SelectionMode::Single);
    // The first column stretches its cells down the rows they cover, so a spanning cell fills its block.
    table.table->setColumns(
        {{.label = "Name", .width = ui::Sizing::fixed(12), .valign = ui::Align::Stretch}, {.label = "Mass"}});
    scene.settle();
    QQuickItem const& tableItem = Scene::itemOf(*table.table);
    QQuickItem const& first = Scene::itemOf(*table.cells.at(0));
    qreal const oneRow = first.height();
    table.table->setRowSpans(*table.rows.at(0), {{.column = 0, .rows = 3}});
    scene.settle();
    auto const [top, bottom] = verticalExtent(first, tableItem);
    QQuickItem const& last = Scene::itemOf(*table.cells.at(5));
    CHECK(first.height() > 2.5 * oneRow);
    // The cell, which a row of its own height would end before, reaches the third row's foot.
    CHECK(bottom >= verticalExtent(last, tableItem).second - 1.0);
    CHECK(top <= verticalExtent(Scene::itemOf(*table.cells.at(1)), tableItem).first + 1.0);
    // The covered rows' cells in that column are hidden, the other columns' are not.
    CHECK_FALSE(Scene::itemOf(*table.cells.at(2)).isVisible());
    CHECK_FALSE(Scene::itemOf(*table.cells.at(4)).isVisible());
    CHECK(Scene::itemOf(*table.cells.at(3)).isVisible());
    // Clearing the span returns everything.
    table.table->setRowSpans(*table.rows.at(0), {});
    scene.settle();
    CHECK(Scene::itemOf(*table.cells.at(2)).isVisible());
    CHECK(first.height() < 1.5 * oneRow);
}

// Mutation: let `cover` write `true` on uncovering (the cell the document hid shows again).
TEST_CASE("qt_quick table: uncovering a cell the document hid leaves it hidden", "[qt_quick][span]") {
    Scene scene;
    Rows table = makeTable(scene, {ui::Key{1}, ui::Key{2}}, ui::SelectionMode::Single);
    table.cells.at(2)->setVisible(false);
    table.table->setRowSpans(*table.rows.at(0), {{.column = 0, .rows = 2}});
    scene.settle();
    CHECK_FALSE(Scene::itemOf(*table.cells.at(2)).isVisible());
    table.table->setRowSpans(*table.rows.at(0), {});
    scene.settle();
    CHECK_FALSE(Scene::itemOf(*table.cells.at(2)).isVisible());
}

// Mutation: leave the backdrop's tap handler out (the click lands on the covered row), or let the row's own handler
// take a tap in a spanning column (the row is toggled twice).
TEST_CASE(
    "qt_quick table: a click on the part of a spanning cell over a covered row selects the row that gives the span",
    "[qt_quick][span]") {
    Scene scene;
    Rows table = makeTable(scene, {ui::Key{1}, ui::Key{2}, ui::Key{3}}, ui::SelectionMode::Multiple);
    std::vector<std::vector<ui::Key>> reported;
    table.table->setOnSelectionChange([&reported](std::vector<ui::Key> keys, ui::SelectionChange /*change*/) {
        reported.push_back(std::move(keys));
    });
    table.table->setRowSpans(*table.rows.at(0), {{.column = 0, .rows = 3}});
    scene.settle();
    // The cell's centre lies over the second row.
    scene.click(Scene::itemOf(*table.cells.at(0)), Qt::ControlModifier);
    REQUIRE(reported.size() == 1);
    CHECK(reported.back() == std::vector<ui::Key>{ui::Key{1}});
    // A click on the covered row's other column is that row's own.
    scene.click(Scene::itemOf(*table.cells.at(3)), Qt::ControlModifier);
    REQUIRE(reported.size() == 2);
    CHECK(reported.back() == std::vector<ui::Key>{ui::Key{1}, ui::Key{2}});
}

// Mutation: leave the backdrop's HoverHandler out (the origin row is not hovered over the part of its cell below its
// bounds), or let it not block (the covered row is hovered as well).
TEST_CASE("qt_quick table: hovering the part of a spanning cell over a covered row hovers the row that gives the span",
          "[qt_quick][span]") {
    Scene scene;
    Rows table = makeTable(scene, {ui::Key{1}, ui::Key{2}, ui::Key{3}}, ui::SelectionMode::Single);
    table.table->setRowSpans(*table.rows.at(0), {{.column = 0, .rows = 3}});
    scene.settle();
    QQuickItem const& origin = Scene::itemOf(*table.rows.at(0));
    QQuickItem const& covered = Scene::itemOf(*table.rows.at(1));
    // Away from the table's rows, nothing is hovered.
    QTest::mouseMove(&scene.window(), QPoint{1, 1});
    scene.settle();
    CHECK_FALSE(origin.property("hovered").toBool());
    // The cell's centre lies over the second row, below the origin row's own bounds.
    QTest::mouseMove(&scene.window(), Scene::centreOf(Scene::itemOf(*table.cells.at(0))));
    scene.settle();
    CHECK(origin.property("hovered").toBool());
    CHECK_FALSE(covered.property("hovered").toBool());
    // Beside the span, the covered row is the hovered one.
    QTest::mouseMove(&scene.window(), Scene::centreOf(Scene::itemOf(*table.cells.at(3))));
    scene.settle();
    CHECK_FALSE(origin.property("hovered").toBool());
    CHECK(covered.property("hovered").toBool());
}

namespace {

// How many rows a viewport of this table shows, plus the one a scroll offset can straddle.
int rowsToFill(QQuickItem const& tableItem, QQuickItem const& flick) {
    return static_cast<int>(std::ceil(flick.height() / tableItem.property("estimatedRowHeight").toReal())) + 1;
}

}  // namespace

// Mutation: drop the report a height change makes in MorphTable.qml (the table then keeps its startup window).
TEST_CASE("qt_quick table: a virtualized table realizes its viewport at mount and again when the viewport grows",
          "[qt_quick]") {
    Scene scene;
    std::vector<VirtualRow> const many = [] {
        std::vector<VirtualRow> rows;
        rows.reserve(2000);
        for (std::int64_t i = 0; i < 2000; ++i) {
            rows.push_back(VirtualRow{.id = i, .name = "row-" + std::to_string(i)});
        }
        return rows;
    }();
    ui::Mounted const mounted{scene.runtime(), scene.backend(),
                              ui::table<VirtualRow>(
                                  {{.label = "Name"}}, [&many] -> std::vector<VirtualRow> const& { return many; },
                                  [](VirtualRow const& row) { return ui::Key{row.id}; }, virtualRowCell,
                                  {.virtualize = {.threshold = 5, .overscan = 1},
                                   .common = {.layout = {.height = ui::Sizing::stretch(1)}}})};
    scene.settle();

    QQuickItem const& tableItem = Scene::itemOf(mounted.root());
    auto* const flick = tableItem.findChild<QQuickItem*>(QStringLiteral("flick"));
    REQUIRE(flick != nullptr);
    REQUIRE(flick->height() > 100);
    REQUIRE(flick->height() < 1000);
    INFO("flick height " << flick->height() << ", realized " << tableItem.property("realizedCount").toInt());
    CHECK(tableItem.property("realizedCount").toInt() >= rowsToFill(tableItem, *flick));

    int const small = tableItem.property("realizedCount").toInt();
    scene.window().resize(800, 1200);
    scene.settle();
    INFO("flick height " << flick->height() << ", realized " << tableItem.property("realizedCount").toInt());
    CHECK(tableItem.property("realizedCount").toInt() > small);
    CHECK(tableItem.property("realizedCount").toInt() >= rowsToFill(tableItem, *flick));
}

// Mutation: drop MorphTable.qml's report on a change of totalRows and its viewport's height.
TEST_CASE("qt_quick table: rows that arrive after the mount fill the viewport without a scroll", "[qt_quick]") {
    Scene scene;
    auto const make = [](std::int64_t count) {
        std::vector<VirtualRow> rows;
        rows.reserve(static_cast<std::size_t>(count));
        for (std::int64_t i = 0; i < count; ++i) {
            rows.push_back(VirtualRow{.id = i, .name = "row-" + std::to_string(i)});
        }
        return rows;
    };
    morph::reactive::Signal<std::vector<VirtualRow>> rows{scene.runtime(), make(3)};
    ui::Mounted const mounted{
        scene.runtime(), scene.backend(),
        ui::table<VirtualRow>(
            {{.label = "Name"}}, [&rows] { return rows.get(); }, [](VirtualRow const& row) { return ui::Key{row.id}; },
            virtualRowCell,
            {.virtualize = {.threshold = 5, .overscan = 1}, .common = {.layout = {.height = ui::Sizing::fixed(20)}}})};
    scene.settle();
    QQuickItem const& tableItem = Scene::itemOf(mounted.root());
    auto* const flick = tableItem.findChild<QQuickItem*>(QStringLiteral("flick"));
    REQUIRE(flick != nullptr);
    CHECK(tableItem.property("totalRows").toInt() == -1);

    rows.set(make(500));
    scene.settle();
    CHECK(tableItem.property("totalRows").toInt() == 500);
    INFO("flick height " << flick->height() << ", realized " << tableItem.property("realizedCount").toInt());
    CHECK(flick->height() > 0);
    CHECK(tableItem.property("realizedCount").toInt() >= rowsToFill(tableItem, *flick));
}

namespace {

// A row 3 in 4 of whose names run to five lines, so the rows' heights differ.
std::vector<VirtualRow> mixedRows(std::int64_t count) {
    std::vector<VirtualRow> rows;
    rows.reserve(static_cast<std::size_t>(count));
    for (std::int64_t i = 0; i < count; ++i) {
        rows.push_back(VirtualRow{.id = i, .name = i % 4 == 0 ? "row\nwith\nfive\nlines\nhere" : "row"});
    }
    return rows;
}

}  // namespace

// Mutation: size the spacers from the realized rows' average (the estimate) instead of the measured heights.
TEST_CASE("qt_quick table: a scrolling virtualized table of mixed row heights moves its rows by the scroll",
          "[qt_quick]") {
    Scene scene;
    std::vector<VirtualRow> const many = mixedRows(600);
    ui::Mounted const mounted{scene.runtime(), scene.backend(),
                              ui::table<VirtualRow>(
                                  {{.label = "Name"}}, [&many] -> std::vector<VirtualRow> const& { return many; },
                                  [](VirtualRow const& row) { return ui::Key{row.id}; }, virtualRowCell,
                                  {.virtualize = {.threshold = 5, .overscan = 3},
                                   .common = {.layout = {.height = ui::Sizing::stretch(1)}}})};
    scene.settle();
    QQuickItem const& tableItem = Scene::itemOf(mounted.root());
    auto* const flick = tableItem.findChild<QQuickItem*>(QStringLiteral("flick"));
    REQUIRE(flick != nullptr);
    auto* const body = tableItem.property("content").value<QQuickItem*>();
    REQUIRE(body != nullptr);

    // The first row whose top is at or below the viewport's top, and where it sits in the viewport.
    auto const topRow = [&]() -> QQuickItem* {
        QQuickItem* best = nullptr;
        for (QQuickItem* const child : body->childItems()) {
            if (child->isVisible() && child->mapToItem(flick, QPointF{0, 0}).y() >= 0 &&
                (best == nullptr || child->y() < best->y())) {
                best = child;
            }
        }
        return best;
    };

    qreal worst = 0;
    int compared = 0;
    for (int step = 0; step < 120; ++step) {
        QPointer<QQuickItem> const row = topRow();
        REQUIRE(!row.isNull());
        qreal const before = row->mapToItem(flick, QPointF{0, 0}).y();
        qreal const delta = 30;
        flick->setProperty("contentY", flick->property("contentY").toReal() + delta);
        scene.settle();
        if (!row.isNull() && row->isVisible()) {
            qreal const moved = before - row->mapToItem(flick, QPointF{0, 0}).y();
            worst = std::max(worst, std::abs(moved - delta));
            ++compared;
        }
    }
    INFO("rows compared " << compared << ", worst deviation from a 30 px scroll " << worst << " px");
    CHECK(compared > 60);
    CHECK(worst < 1.0);
}

// Mutation: do not create the vertical bar in MorphTable.qml.
TEST_CASE("qt_quick table: a virtualized table scrolls with a vertical bar sized by its content", "[qt_quick]") {
    Scene scene;
    std::vector<VirtualRow> const many = mixedRows(600);
    ui::Mounted const mounted{scene.runtime(), scene.backend(),
                              ui::table<VirtualRow>(
                                  {{.label = "Name"}}, [&many] -> std::vector<VirtualRow> const& { return many; },
                                  [](VirtualRow const& row) { return ui::Key{row.id}; }, virtualRowCell,
                                  {.virtualize = {.threshold = 5, .overscan = 3},
                                   .common = {.layout = {.height = ui::Sizing::stretch(1)}}})};
    scene.settle();
    QQuickItem const& tableItem = Scene::itemOf(mounted.root());
    auto* const flick = tableItem.findChild<QQuickItem*>(QStringLiteral("flick"));
    REQUIRE(flick != nullptr);
    auto const bars = Scene::itemsOf(*flick, "QQuickScrollBar");
    INFO("scroll bars under the flickable: " << bars.size());
    REQUIRE(bars.size() == 1);
    QQuickItem const* const vertical = bars.front();
    CHECK(vertical->isVisible());
    CHECK(vertical->property("orientation").toInt() == Qt::Vertical);
    CHECK(vertical->property("size").toReal() < 0.5);
    CHECK(vertical->property("minimumSize").toReal() == Catch::Approx(0.08));

    // A table that is not virtualized has no vertical bar: its ancestor scrolls.
    std::vector<VirtualRow> const few = mixedRows(3);
    ui::Mounted const small{scene.runtime(), scene.backend(),
                            ui::table<VirtualRow>(
                                {{.label = "Name"}}, [&few] -> std::vector<VirtualRow> const& { return few; },
                                [](VirtualRow const& row) { return ui::Key{row.id}; }, virtualRowCell,
                                {.virtualize = {.threshold = 5, .overscan = 3}})};
    scene.settle();
    auto* const smallFlick = Scene::itemOf(small.root()).findChild<QQuickItem*>(QStringLiteral("flick"));
    REQUIRE(smallFlick != nullptr);
    for (QQuickItem const* const candidate : Scene::itemsOf(*smallFlick, "QQuickScrollBar")) {
        CHECK_FALSE(candidate->isVisible());
    }
}

// Mutation: build a Ctrl-click's selection from the realized rows alone instead of from every key the data holds.
TEST_CASE("qt_quick table: a Ctrl-click in a virtualized table keeps selected keys whose rows are not realized",
          "[qt_quick]") {
    Scene scene;
    constexpr std::int64_t rowCount = 40;
    std::vector<VirtualRow> const many = [] {
        std::vector<VirtualRow> rows;
        rows.reserve(rowCount);
        for (std::int64_t index = 0; index < rowCount; ++index) {
            rows.push_back(VirtualRow{.id = index, .name = "row-" + std::to_string(index)});
        }
        return rows;
    }();
    std::vector<std::vector<ui::Key>> reported;
    ui::Mounted const mounted{scene.runtime(), scene.backend(),
                              ui::table<VirtualRow>(
                                  {{.label = "Name"}}, [&many] { return std::vector<VirtualRow>{many}; },
                                  [](VirtualRow const& row) { return ui::Key{row.id}; }, virtualRowCell,
                                  {.virtualize = {.threshold = 5, .overscan = 1},
                                   .selectionMode = ui::SelectionMode::Multiple,
                                   .onSelectionChange = [&reported](std::vector<ui::Key> const& keys,
                                                                    ui::SelectionChange) { reported.push_back(keys); },
                                   .common = {.layout = {.height = ui::Sizing::fixed(10)}}})};
    scene.settle();
    auto const* const core = dynamic_cast<detail::ContainerCore const*>(&mounted.root());
    REQUIRE(core != nullptr);
    auto const& rows = core->children();
    REQUIRE_FALSE(rows.empty());
    REQUIRE(rows.size() < static_cast<std::size_t>(rowCount));

    std::vector<ui::Key> all;
    all.reserve(rowCount);
    for (std::int64_t index = 0; index < rowCount; ++index) {
        all.emplace_back(index);
    }
    auto* const table = dynamic_cast<detail::TableImpl*>(&mounted.root());
    REQUIRE(table != nullptr);
    table->setSelection(all);
    scene.settle();

    scene.click(*rows.at(0)->item(), Qt::ControlModifier);
    REQUIRE(reported.size() == 1);
    CHECK(reported.back().size() == static_cast<std::size_t>(rowCount - 1));
}

// ── The column window ───────────────────────────────────────────────────────

namespace {

using morph::testing::FakeRows;
using ui::CellRef;
using ui::Key;
using ui::TableArrangement;

struct QuickWindowOptions {
    // A cell drawn on more lines than one: the row's text, then a line more for each line past the first.
    struct Wrapped {
        std::int64_t row = 0;
        std::size_t column = 0;
        int lines = 1;
    };
    struct Level {
        std::size_t column = 0;
        std::string level;
    };
    std::size_t pinned = 0;
    std::vector<Wrapped> wrapped;
    bool wrap = true;
    std::vector<Level> levels;
    ui::SelectionMode selection = ui::SelectionMode::None;
    bool selectColumn = false;
    // Each column's unit, by index; a column past the list has none.
    std::vector<std::string> units;
    // Whether the table has a select-all handler, which counts its calls in `selectAlls`.
    bool selectAllHandler = false;
    // The cells' menu; empty for none.
    std::function<std::vector<ui::MenuItem>(ui::Key const&, std::size_t)> cellMenu;
};

std::string groupedCount(std::int64_t n) {
    std::string digits = std::to_string(n);
    for (auto at = static_cast<std::ptrdiff_t>(digits.size()) - 3; at > 0; at -= 3) {
        digits.insert(static_cast<std::size_t>(at), ",");
    }
    return digits;
}

std::string fieldText(ui::Value const& record, std::string const& field) {
    ui::Value const* const value = record.field(field);
    return value != nullptr && value->asString() != nullptr ? *value->asString() : std::string{};
}

std::int64_t idOf(ui::Value const& record) {
    ui::Value const* const value = record.field("id");
    return value != nullptr && value->asInt() != nullptr ? *value->asInt() : -1;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the column count, then the column left out.
std::vector<std::size_t> allColumnsBut(std::size_t n, std::size_t left) {
    std::vector<std::size_t> out;
    for (std::size_t column = 0; column < n; ++column) {
        if (column != left) {
            out.push_back(column);
        }
    }
    return out;
}

// The recording backend's `WindowFixture` on Qt Quick: one table over `FakeRows` in a 1,280 × 800 window, every
// column 8 units wide with the default text cell, and the window made twenty columns wide (`column` px each) once the
// mount has laid the columns out in the table's measure; the status line shows the count.
struct QuickWindowFixture {
    QuickWindowFixture(std::int64_t rowCount, std::size_t columnCount, QuickWindowOptions options = {})
        : rows{std::make_shared<FakeRows>(rowCount, columnCount, scene.owner())},
          arrangement{scene.runtime(), TableArrangement{.visible = {}, .pinned = options.pinned, .widths = {}}},
          focus{scene.runtime(), std::nullopt},
          status{scene.runtime(), ui::TableStatus{.counts = groupedCount(rowCount) + " rows", .shown = true}},
          accents{scene.runtime(), {}},
          selected{scene.runtime(), {}},
          wrapped{std::move(options.wrapped)} {
        scene.window().resize(1280, 800);
        std::vector<ui::TableColumn> columns;
        columns.reserve(columnCount);
        for (std::size_t index = 0; index < columnCount; ++index) {
            columns.push_back(
                ui::TableColumn{.id = "c" + std::to_string(index),
                                .label = "C" + std::to_string(index),
                                .width = ui::Sizing::fixed(8),
                                .unit = index < options.units.size() ? options.units.at(index) : std::string{},
                                .wrap = options.wrap});
        }
        ui::TableSource source;
        source.rows = rows;
        source.ownCell.assign(columnCount, false);
        source.levels.assign(columnCount, std::string{});
        for (QuickWindowOptions::Level const& level : options.levels) {
            source.levels.at(level.column) = level.level;
        }
        source.defaultCellView = [this](morph::reactive::Signal<std::size_t> const& cellColumn,
                                        morph::reactive::Signal<ui::Value> const& row) {
            return ui::accentedCell(
                ui::text({.text = [this, &cellColumn, &row] { return textOf(row.get(), cellColumn.get()); }}),
                [this, &cellColumn, &row] {
                    auto const& all = accents.get();
                    auto const found = all.find(std::pair{idOf(row.get()), cellColumn.get()});
                    return found == all.end() ? ui::CellAccent{} : found->second;
                });
        };
        source.cellView = [this](std::size_t cellColumn, morph::reactive::Signal<ui::Value> const& row) {
            return ui::text({.text = [this, &row, cellColumn] { return textOf(row.get(), cellColumn); }});
        };
        ui::TableOptions tableOptions;
        // A table over a source is a viewport onto rows it does not hold: it takes the window's height.
        tableOptions.common.layout.height = ui::Sizing::stretch();
        tableOptions.arrangement = [this] { return arrangement.get(); };
        tableOptions.focus = [this] { return focus.get(); };
        tableOptions.status = [this] { return status.get(); };
        tableOptions.onRetry = [this] { ++retries; };
        tableOptions.selectionMode = options.selection;
        tableOptions.selectColumn = options.selectColumn;
        tableOptions.onSelectRange = [this](std::size_t anchor, std::size_t focusAt, ui::SelectionChange change) {
            ranges.emplace_back(anchor, focusAt, change);
        };
        if (options.selectAllHandler) {
            tableOptions.onSelectAll = [this] { ++selectAlls; };
        }
        // The selection is held here, as a document holds it: each change the user makes is recorded and kept.
        tableOptions.selection = [this] { return selected.get(); };
        tableOptions.onSelectionChange = [this](std::vector<ui::Key> keys, ui::SelectionChange change) {
            selections.emplace_back(keys, change);
            selected.set(std::move(keys));
        };
        tableOptions.cellMenu = std::move(options.cellMenu);
        view.emplace(scene.runtime(), scene.backend(),
                     ui::table(std::move(columns), std::move(source), std::move(tableOptions)));
        settle();
        column = static_cast<int>(call("columnWidth", 0).toReal());
        REQUIRE(column > 0);
        scene.window().resize(20 * column, 800);
        settle();
    }

    [[nodiscard]] std::string textOf(ui::Value const& record, std::size_t cellColumn) const {
        std::string text = fieldText(record, "c" + std::to_string(cellColumn));
        for (QuickWindowOptions::Wrapped const& cell : wrapped) {
            if (cell.row == idOf(record) && cell.column == cellColumn) {
                for (int line = 1; line < cell.lines; ++line) {
                    text += "\n" + text;
                }
            }
        }
        return text;
    }

    void settle() { scene.settle(); }

    [[nodiscard]] QQuickItem* item() const { return &Scene::itemOf(view->root()); }
    [[nodiscard]] QVariant prop(char const* name) const { return item()->property(name); }

    // Evaluates `name(args…)` on the MorphTable item.
    template <class... Args>
    QVariant call(char const* name, Args const&... args) {
        // NOLINTNEXTLINE(misc-const-correctness): the fold expression below appends to it, which the check does not see.
        QStringList list;
        (list.append(QString::number(args)), ...);
        QQmlExpression expression{
            scene.engine().rootContext(), item(),
            QStringLiteral("%1(%2)").arg(QString::fromLatin1(name), list.join(QStringLiteral(",")))};
        QVariant const result = expression.evaluate();
        REQUIRE_FALSE(expression.hasError());
        return result;
    }

    // The cell items shown in the realized rows.
    int cellItems() {
        QQmlExpression expression{scene.engine().rootContext(), item(), QStringLiteral(R"((() => {
            let shown = 0
            for (let i = 0; i < rowsHolder.children.length; ++i) {
                const row = rowsHolder.children[i]
                if (!row.visible || !row.content)
                    continue
                for (let j = 0; j < row.content.children.length; ++j) {
                    const cell = row.content.children[j]
                    shown += cell.measuredIndex !== undefined && cell.visible ? 1 : 0
                }
            }
            return shown
        })())")};
        QVariant const result = expression.evaluate();
        REQUIRE_FALSE(expression.hasError());
        return result.toInt();
    }

    void scrollRows(std::size_t first) {
        firstRow = first;
        static_cast<void>(call("scrollRowToTop", static_cast<qlonglong>(first)));
        settle();
    }

    void scrollColumns(int scrollX) {
        auto* const hflick = item()->findChild<QQuickItem*>(QStringLiteral("hflick"));
        REQUIRE(hflick != nullptr);
        hflick->setProperty("contentX", scrollX);
        settle();
    }

    // Scrolls down over @p n rows a viewport at a time, one owner turn at a time, and keeps the largest number of
    // cells built or rebound in one turn.
    void flingRows(std::size_t n) {
        auto const work = [this] { return stats().cellsRebound + stats().cellsCreated; };
        for (std::size_t moved = 0; moved < n; moved += 30) {
            std::size_t before = work();
            firstRow += 30;
            static_cast<void>(call("scrollRowToTop", static_cast<qlonglong>(firstRow)));
            maxRebindsPerTurn = std::max(maxRebindsPerTurn, work() - before);
            for (;;) {
                before = work();
                if (!scene.owner().runOne()) {
                    break;
                }
                maxRebindsPerTurn = std::max(maxRebindsPerTurn, work() - before);
            }
        }
        settle();
    }

    [[nodiscard]] ui::MountStats const& stats() const { return view->stats(); }

    Scene scene;
    std::shared_ptr<FakeRows> rows;
    morph::reactive::Signal<TableArrangement> arrangement;
    morph::reactive::Signal<std::optional<CellRef>> focus;
    morph::reactive::Signal<ui::TableStatus> status;
    // Each cell's accent, by row id and column; a cell not named has none.
    morph::reactive::Signal<std::map<std::pair<std::int64_t, std::size_t>, ui::CellAccent>> accents;
    // The selected keys, and every change the backend reported.
    morph::reactive::Signal<std::vector<ui::Key>> selected;
    std::vector<std::pair<std::vector<ui::Key>, ui::SelectionChange>> selections;
    std::vector<QuickWindowOptions::Wrapped> wrapped;
    int retries = 0;
    int selectAlls = 0;
    std::vector<std::tuple<std::size_t, std::size_t, ui::SelectionChange>> ranges;
    std::size_t firstRow = 0;
    std::size_t maxRebindsPerTurn = 0;
    int column = 0;  ///< A column's width in pixels, the measure of a fixed column of 8 units and its padding.
    std::optional<ui::Mounted> view;
};

}  // namespace

// Mutation: MorphTable instantiating a cell per column of a realized row (cellItems is 50 × 300).  Gate F2.
TEST_CASE("qt_quick table window: cells and header cells alive are the windows'", "[qt_quick][heavy]") {
    QuickWindowFixture f{100'000, 300, {.pinned = 2}};
    f.scrollRows(500);
    f.scrollColumns(100 * f.column);  // columns 100 to 119 in view
    int const window = 2 + 20 + (2 * 4);
    CHECK(f.prop("realizedColumnCount").toInt() == window);
    CHECK(f.prop("pinnedShown").toInt() == 2);
    CHECK(f.cellItems() == f.prop("realizedCount").toInt() * window);
    CHECK(f.prop("headerCellCount").toInt() == window);
}

// Mutation: a pool keyed by column for default cells (a column scrolling in creates 50 cells).  Gate F8.
TEST_CASE("qt_quick table window: flings create no cell once the pools are warm", "[qt_quick][heavy]") {
    QuickWindowFixture f{100'000, 300, {.pinned = 2}};
    // Two flings warm the rows: the first measures the rows it passes, and the mean height they settle at can put one
    // more row in the window than the estimate did, which only the second fling's pool has to hold.
    f.flingRows(2'000);
    f.flingRows(2'000);
    for (int x = 0; x < 200 * f.column; x += 10 * f.column) {
        f.scrollColumns(x);  // warm both ways
    }
    auto const created = f.stats().cellsCreated;
    f.flingRows(2'000);
    for (int x = 200 * f.column; x < 300 * f.column; x += 10 * f.column) {
        f.scrollColumns(x);
    }
    CHECK(f.stats().cellsCreated == created);
    CHECK(f.maxRebindsPerTurn <= 240);
}

// Mutation: MorphTable keeping focus on the delegate item, which a recycled row loses.  Gate F10.
TEST_CASE("qt_quick table window: focus and counts after scrolling far away", "[qt_quick][heavy]") {
    QuickWindowFixture f{100'000, 300, {.pinned = 2}};
    f.focus.set(CellRef{.key = Key{std::int64_t{7}}, .column = "c5"});
    f.settle();
    f.scrollRows(10'000);
    f.scrollColumns(200 * f.column);
    CHECK(f.call("focusedRow").toInt() == 7);
    CHECK(f.call("focusedColumn").toInt() == 5);
    CHECK(f.call("cellRealized", 7, 5).toBool());
    CHECK(f.prop("accessibleRowCount").toInt() == 100'000);
    CHECK(f.prop("accessibleColumnCount").toInt() == 300);
}

// Mutation: MorphTable rebuilding its row delegates when the column window changes.  Gate D8, Qt Quick half.
TEST_CASE("qt_quick table window: hiding and moving a column create no cell items", "[qt_quick][heavy]") {
    QuickWindowFixture f{100'000, 300, {.pinned = 2}};
    auto const created = f.stats().cellsCreated;
    int const items = f.cellItems();
    TableArrangement hidden = f.arrangement.peek();
    hidden.visible = allColumnsBut(300, 5);
    f.arrangement.set(hidden);
    f.settle();
    CHECK(f.stats().cellsCreated == created);
    CHECK(f.cellItems() == items);
    CHECK_FALSE(f.call("cellRealized", 0, 5).toBool());
    TableArrangement moved = hidden;
    std::swap(moved.visible.at(3), moved.visible.at(4));
    f.arrangement.set(moved);
    f.settle();
    CHECK(f.stats().cellsCreated == created);
    CHECK(f.cellItems() == items);
}

// Mutation: MorphTable ignoring setStatus (statusText is empty).
TEST_CASE("qt_quick table: the status line shows counts and Retry", "[qt_quick]") {
    QuickWindowFixture f{100'000, 30};
    CHECK(f.prop("statusText").toString() == QStringLiteral("100,000 rows"));
    CHECK_FALSE(f.prop("retryShown").toBool());
    f.status.set(ui::TableStatus{
        .counts = "100,000 rows", .state = "Recipe data could not be loaded.", .retry = true, .shown = true});
    f.settle();
    CHECK(f.prop("statusText").toString() == QStringLiteral("100,000 rows  Recipe data could not be loaded."));
    REQUIRE(f.prop("retryShown").toBool());
    auto* const retry = f.item()->findChild<QQuickItem*>(QStringLiteral("retry"));
    REQUIRE(retry != nullptr);
    f.scene.click(*retry);
    CHECK(f.retries == 1);
}

// Mutation: MorphTable ignoring setCellLoad (an unloaded cell draws its empty content: cellLoadAt is 0).
TEST_CASE("qt_quick table: an unloaded cell is a placeholder and a failed one an error mark", "[qt_quick]") {
    QuickWindowFixture f{100, 4, {.levels = {{.column = 1, .level = "recipe"}}}};
    int const widthBefore = f.call("columnWidth", 1).toInt();
    f.rows->unload(0, "recipe");
    f.rows->fail(1, "recipe");
    f.settle();
    CHECK(f.call("cellLoadAt", 0, 1).toInt() == 1);
    CHECK(f.call("cellLoadAt", 1, 1).toInt() == 2);
    CHECK(f.call("cellLoadAt", 2, 1).toInt() == 0);
    CHECK(f.call("columnWidth", 1).toInt() == widthBefore);  // a placeholder widens nothing
}

// Mutations: the tint painted opaque over the row's selection fill (its alpha is 1); the slot under the row's
// background (z -1), where a toned or selected row hides it; `invalid` ignored by
// MorphTableRow.qml, which draws no error mark (cellLoadAt is 0 and no failedMark exists).
TEST_CASE(
    "qt_quick table: a toned cell keeps its selection mark, a tooltip cell is described, a unit header is named "
    "with its unit",
    "[qt_quick]") {
    QuickWindowFixture f{10, 3, {.units = {"kg"}}};
    auto* const header = itemNamed(*f.item(), QStringLiteral("header0"));
    REQUIRE(header != nullptr);
    CHECK(header->property("text").toString() == QStringLiteral("C0 [kg]"));
    bool const wasActive = QAccessible::isActive();
    QAccessible::setActive(true);
    QAccessibleInterface const* const named = QAccessible::queryAccessibleInterface(header);
    REQUIRE(named != nullptr);
    CHECK(named->text(QAccessible::Name) == QStringLiteral("C0, kg"));

    f.accents.set({{{0, 0}, ui::CellAccent{.tone = ui::Tone::Warn, .tooltip = "Above limit", .invalid = false}},
                   {{0, 1}, ui::CellAccent{.tone = {}, .tooltip = "The value could not be read", .invalid = true}}});
    f.settle();
    QVariant const accent = f.call("cellAccentAt", 0, 0);
    REQUIRE(accent.isValid());
    auto* const tinted = itemNamed(*f.item(), QStringLiteral("cellAccent0"));
    REQUIRE(tinted != nullptr);
    auto* const tint = itemNamed(*tinted, QStringLiteral("cellTint"));
    REQUIRE(tint != nullptr);
    CHECK(tint->isVisible());
    CHECK(itemNamed(*tinted, QStringLiteral("moreMark"))->isVisible());
    QAccessibleInterface const* const described = QAccessible::queryAccessibleInterface(tinted);
    REQUIRE(described != nullptr);
    CHECK(described->text(QAccessible::Description) == QStringLiteral("Above limit"));
    // Half the tone's fill: the row's selection fill and bar show through it.
    CHECK(tint->property("color").value<QColor>().alphaF() < 1.0F);
    // Over the row's background, whose tone tint and selection fill are opaque: after it among the row's children, at
    // no lower z, so a toned cell keeps its tone in a toned or selected row.
    QQuickItem* const row = tinted->parentItem();
    REQUIRE(row != nullptr);
    QQuickItem* layer = itemNamed(*row, QStringLiteral("fill"));
    while (layer != nullptr && layer->parentItem() != row) {
        layer = layer->parentItem();
    }
    REQUIRE(layer != nullptr);
    CHECK(tinted->z() >= layer->z());
    CHECK(row->childItems().indexOf(tinted) > row->childItems().indexOf(layer));
    CHECK(f.call("cellLoadAt", 0, 1).toInt() == 0);  // the load is the mount's; the mark is the accent's
    CHECK(itemNamed(*f.item(), QStringLiteral("failedMark")) != nullptr);
    QAccessible::setActive(wasActive);

    f.accents.set({});
    f.settle();
    CHECK(itemNamed(*f.item(), QStringLiteral("cellAccent0")) == nullptr);
    CHECK(itemNamed(*f.item(), QStringLiteral("failedMark")) == nullptr);
}

// Mutations: a row's height recomputed from its realized cells each pass (it shrinks when the tall cell leaves);
// measuring a table whose columns are all wrap:false (rowMeasures grows).
TEST_CASE("qt_quick table: a row's height only grows while it is realized", "[qt_quick][heavy]") {
    QuickWindowFixture f{2'000, 300, {.pinned = 0, .wrapped = {{.row = 3, .column = 60, .lines = 2}}}};
    // Before the first viewport report every column is realized, the wrapped one too; a row that left the row window
    // and came back has measured only the columns in view.
    f.scrollRows(1'000);
    f.scrollRows(0);
    int const low = f.call("rowHeight", 3).toInt();
    f.scrollColumns(60 * f.column);
    int const tall = f.call("rowHeight", 3).toInt();
    CHECK(tall > low);
    f.scrollColumns(0);
    CHECK(f.call("rowHeight", 3).toInt() == tall);  // scrolled out: not shorter
    f.scrollRows(1'000);
    f.scrollRows(0);
    CHECK(f.call("rowHeight", 3).toInt() == low);  // it left the row window: forgotten
    QuickWindowFixture flat{2'000, 300, {.pinned = 0, .wrap = false}};
    flat.flingRows(200);
    CHECK(flat.prop("rowMeasures").toInt() == 0);
}

// Mutation: MorphTable keeping the view's offset when a row above the first visible row grows (the first visible
// row moves down by the growth).
TEST_CASE("qt_quick table: a row growing above the view keeps the rows in view where they are", "[qt_quick][heavy]") {
    QuickWindowFixture f{2'000, 300, {.pinned = 0, .wrapped = {{.row = 95, .column = 60, .lines = 3}}}};
    f.scrollRows(100);
    QQuickItem const* const flick = f.item()->findChild<QQuickItem*>(QStringLiteral("flick"));
    REQUIRE(flick != nullptr);
    qreal const offsetBefore = flick->property("contentY").toReal() - f.call("rowTop", 100).toReal();
    f.scrollColumns(60 * f.column);  // row 95, above the view and realized, grows
    CHECK(f.call("rowHeight", 95).toReal() > f.call("rowHeight", 96).toReal());
    CHECK(flick->property("contentY").toReal() - f.call("rowTop", 100).toReal() == Catch::Approx(offsetBefore));
}

// Mutation: the Qt Quick table answering the default measure (a windowed column of 8 units is 8 px plus nothing).
TEST_CASE("qt_quick table: a windowed fixed column is as wide as an unwindowed one", "[qt_quick][heavy]") {
    QuickWindowFixture windowed{100'000, 300};
    REQUIRE(windowed.prop("arranged").toBool());
    Scene scene;
    ui::Mounted const plain{
        scene.runtime(), scene.backend(),
        ui::table<std::int64_t>(
            {{.label = "C0", .width = ui::Sizing::fixed(8)}}, [] { return std::vector<std::int64_t>{1, 2}; },
            [](std::int64_t const& row) { return ui::Key{row}; },
            [](morph::reactive::Signal<std::int64_t> const& row) {
                return std::vector<ui::Node>{ui::text({.text = [&row] { return std::to_string(row.get()); }})};
            })};
    scene.settle();
    QVariantList const widths = Scene::itemOf(plain.root()).property("widths").toList();
    REQUIRE(widths.size() == 1);
    CHECK(windowed.call("columnWidth", 0).toReal() == Catch::Approx(widths.at(0).toReal()).margin(1.0));
}

// Mutations: a tap on a row of a source table focusing the row (the table loses `activeFocus`); MorphTable announcing
// nothing (no announcement event reaches the accessibility handler).
TEST_CASE("qt_quick table window: a tap keeps the focus on the table, and the status line is announced",
          "[qt_quick]") {
    QuickWindowFixture f{100'000, 30};
    QQuickItem const* const row = f.item()->property("rowsHolder").value<QQuickItem*>()->childItems().value(3);
    REQUIRE(row != nullptr);
    f.scene.click(*row);
    CHECK(f.item()->hasActiveFocus());
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    // Announcements are Qt 6.8's.
    static std::vector<QString> announced;
    announced.clear();
    QAccessible::installUpdateHandler([](QAccessibleEvent* event) {
        if (event->type() == QAccessible::Announcement) {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the event type was checked above.
            announced.push_back(static_cast<QAccessibleAnnouncementEvent*>(event)->message());
        }
    });
    QAccessible::setActive(true);
    f.status.set(ui::TableStatus{.counts = "100,000 rows", .message = "Copied 2 rows", .shown = true});
    f.settle();
    QTest::qWait(700);  // the announcement waits for the text to hold half a second
    QAccessible::installUpdateHandler(nullptr);
    CHECK(std::ranges::contains(announced, QStringLiteral("100,000 rows  Copied 2 rows")));
#endif
}

// Mutation: the select-all box over a source selecting the realized rows (`TableImpl::selectAll`: no range reaches
// the mount).
TEST_CASE("qt_quick table window: the select-all box selects the view through the mount", "[qt_quick]") {
    QuickWindowFixture f{100'000, 30, {.selection = ui::SelectionMode::Multiple, .selectColumn = true}};
    auto* const box = itemNamed(*f.item(), QStringLiteral("selectAll"));
    REQUIRE(box != nullptr);
    f.scene.click(*box);
    REQUIRE_FALSE(f.ranges.empty());
    CHECK(f.ranges.back() == std::tuple{std::size_t{0}, std::size_t{99'999}, ui::SelectionChange::Extend});
}

// Mutation: Shift with an arrow over a source handled by the realized rows (no range reaches the mount).
TEST_CASE("qt_quick table window: Shift with an arrow and Ctrl+A select through the mount", "[qt_quick]") {
    QuickWindowFixture f{100'000, 30, {.selection = ui::SelectionMode::Multiple}};
    f.item()->forceActiveFocus();
    f.scene.key(Qt::Key_Down);
    f.scene.key(Qt::Key_Down, Qt::ShiftModifier);
    f.scene.key(Qt::Key_Down, Qt::ShiftModifier);
    REQUIRE_FALSE(f.ranges.empty());
    CHECK(f.ranges.back() == std::tuple{std::size_t{1}, std::size_t{3}, ui::SelectionChange::Replace});
    f.scene.key(Qt::Key_A, Qt::ControlModifier);
    CHECK(f.ranges.back() == std::tuple{std::size_t{0}, std::size_t{99'999}, ui::SelectionChange::Extend});
}

// Mutation: `TableImpl::setOnSelectAll` not writing `selectAllHandled` (Ctrl+A and the box fire a range, and the
// handler is never called).
TEST_CASE("qt_quick table window: a select-all handler takes Ctrl+A and the select-all box over a source",
          "[qt_quick]") {
    QuickWindowFixture f{
        100'000, 30, {.selection = ui::SelectionMode::Multiple, .selectColumn = true, .selectAllHandler = true}};
    CHECK(f.prop("selectAllHandled").toBool());
    f.item()->forceActiveFocus();
    f.scene.key(Qt::Key_A, Qt::ControlModifier);
    CHECK(f.selectAlls == 1);
    auto* const box = itemNamed(*f.item(), QStringLiteral("selectAll"));
    REQUIRE(box != nullptr);
    f.scene.click(*box);
    CHECK(f.selectAlls == 2);
    CHECK(f.ranges.empty());
}

// Control: leave the menu popup without the focus (the arrows and Enter go to the header, and Escape does not close
// it).
TEST_CASE("qt_quick table: a header menu opened from the keyboard takes the arrows, Enter and Escape",
          "[qt_quick][table]") {
    Scene scene;
    Rows table = makeTable(scene, {ui::Key{std::int64_t{1}}}, ui::SelectionMode::None);
    table.table->setColumns({{.label = "Name", .width = ui::Sizing::fixed(12)}, {.label = "Mass"}});
    std::vector<std::pair<std::optional<std::size_t>, std::vector<std::size_t>>> chosen;
    table.table->setMenu(
        [](std::optional<std::size_t>) {
            return std::vector<ui::MenuEntry>{
                {.label = "Sort ascending"}, {.separator = true}, {.label = "Hide column"}};
        },
        [&chosen](std::optional<std::size_t> column, std::vector<std::size_t> path) {
            chosen.emplace_back(column, std::move(path));
        });
    scene.settle();
    QQuickItem& name = headerOf(*table.table, 0);
    auto* const menu = Scene::itemOf(*table.table).findChild<QObject*>(QStringLiteral("configMenu"));
    REQUIRE(menu != nullptr);
    name.forceActiveFocus();
    scene.key(Qt::Key_F10, Qt::ShiftModifier);
    REQUIRE(menu->property("opened").toBool());
    // Down passes over the separator to Hide column; Enter chooses it.
    scene.key(Qt::Key_Down);
    scene.key(Qt::Key_Return);
    CHECK_FALSE(menu->property("opened").toBool());
    REQUIRE(chosen.size() == 1);
    CHECK(chosen.front().first == std::optional<std::size_t>{0});
    CHECK(chosen.front().second == std::vector<std::size_t>{2});
    CHECK(name.hasActiveFocus());
    // Opened with the mouse, Escape closes it and chooses nothing.
    QTest::mouseClick(&scene.window(), Qt::RightButton, {}, Scene::centreOf(name));
    scene.settle();
    REQUIRE(menu->property("opened").toBool());
    scene.key(Qt::Key_Escape);
    CHECK_FALSE(menu->property("opened").toBool());
    CHECK(chosen.size() == 1);
}

// Control: draw a menu entry's label alone (the checked entry reads `Wrap`, and assistive technology hears no check).
TEST_CASE("qt_quick table: a header menu shows a checked entry's mark, its chord and its description, and states it",
          "[qt_quick][table]") {
    Scene scene;
    Rows table = makeTable(scene, {ui::Key{std::int64_t{1}}}, ui::SelectionMode::None);
    table.table->setColumns({{.label = "Name", .width = ui::Sizing::fixed(12)}, {.label = "Mass"}});
    table.table->setMenu(
        [](std::optional<std::size_t>) {
            return std::vector<ui::MenuEntry>{
                {.label = "Wrap", .keys = "Ctrl+W", .checked = true, .description = "Long text wraps"},
                {.separator = true},
                {.label = "Pin", .checked = false}};
        },
        [](std::optional<std::size_t>, std::vector<std::size_t> const&) {});
    scene.settle();
    QQuickItem const& name = headerOf(*table.table, 0);
    QTest::mouseClick(&scene.window(), Qt::RightButton, {}, Scene::centreOf(name));
    scene.settle();
    auto* const menu = Scene::itemOf(*table.table).findChild<QObject*>(QStringLiteral("configMenu"));
    REQUIRE(menu != nullptr);
    REQUIRE(menu->property("opened").toBool());
    auto* const entries = menu->property("contentItem").value<QQuickItem*>();
    REQUIRE(entries != nullptr);
    QQuickItem* wrap = nullptr;
    QQuickItem* pin = nullptr;
    QStringList texts;
    QList<QQuickItem*> pending{entries};
    while (!pending.isEmpty()) {
        QQuickItem* const item = pending.takeLast();
        if (item->objectName() == QStringLiteral("menuEntry0")) {
            wrap = item;
        } else if (item->objectName() == QStringLiteral("menuEntry2")) {
            pin = item;
        }
        if (item->isVisible() && !item->property("text").toString().isEmpty()) {
            texts.append(item->property("text").toString());
        }
        pending.append(item->childItems());
    }
    REQUIRE(wrap != nullptr);
    REQUIRE(pin != nullptr);
    CHECK(wrap->property("text").toString() == QStringLiteral("\u2713 Wrap    Ctrl+W"));
    CHECK(pin->property("text").toString() == QStringLiteral("  Pin"));
    CHECK(texts.contains(QStringLiteral("Long text wraps")));
    bool const wasActive = QAccessible::isActive();
    QAccessible::setActive(true);
    QAccessibleInterface const* const wrapNode = QAccessible::queryAccessibleInterface(wrap);
    QAccessibleInterface const* const pinNode = QAccessible::queryAccessibleInterface(pin);
    REQUIRE(wrapNode != nullptr);
    REQUIRE(pinNode != nullptr);
    CHECK(wrapNode->role() == QAccessible::MenuItem);
    CHECK(wrapNode->state().checkable);
    CHECK(wrapNode->state().checked);
    CHECK(pinNode->state().checkable);
    CHECK_FALSE(pinNode->state().checked);
    QAccessible::setActive(wasActive);
    scene.key(Qt::Key_Escape);
}

// Mutation: the menu opened without moving focus (the cell menu's entries not focusing the cell, so the focused row
// stays -1).
TEST_CASE("qt_quick table: a right-click opens the cell menu on that cell and selects its row alone",
          "[qt_quick][table]") {
    QuickWindowOptions options{.selection = ui::SelectionMode::Multiple};
    options.cellMenu = [](ui::Key const&, std::size_t) {
        std::vector<ui::MenuItem> items(2);
        items.at(0).label = std::string{"Open"};
        items.at(1).label = std::string{"Close"};
        return items;
    };
    QuickWindowFixture f{10, 3, std::move(options)};
    f.selected.set({ui::Key{std::int64_t{1}}, ui::Key{std::int64_t{3}}});
    f.settle();
    QQmlExpression find{f.scene.engine().rootContext(), f.item(), QStringLiteral(R"((() => {
        const row = realizedRow(2)
        if (!row)
            return null
        for (let i = 0; i < row.content.children.length; ++i) {
            const cell = row.content.children[i]
            if (cell.measuredIndex === 1 && cell.visible)
                return cell
        }
        return null
    })())")};
    auto* const cell = qobject_cast<QQuickItem*>(find.evaluate().value<QObject*>());
    REQUIRE_FALSE(find.hasError());
    REQUIRE(cell != nullptr);
    QTest::mouseClick(&f.scene.window(), Qt::RightButton, {}, Scene::centreOf(*cell));
    f.settle();
    REQUIRE_FALSE(f.selections.empty());
    CHECK(f.selections.back() ==
          std::pair{std::vector<ui::Key>{ui::Key{std::int64_t{2}}}, ui::SelectionChange::Replace});
    CHECK(f.call("focusedRow").toInt() == 2);
    CHECK(f.call("focusedColumn").toInt() == 1);
    auto* const menu = f.item()->findChild<QObject*>(QStringLiteral("configMenu"));
    REQUIRE(menu != nullptr);
    REQUIRE(menu->property("opened").toBool());
    auto const labels = [menu] {
        QStringList out;
        for (QVariant const& entry : menu->property("entries").toList()) {
            out.append(entry.toMap().value(QStringLiteral("label")).toString());
        }
        return out;
    };
    QStringList const expected{QStringLiteral("Open"), QStringLiteral("Close")};
    CHECK(labels() == expected);
    f.scene.key(Qt::Key_Escape);
    f.settle();
    REQUIRE_FALSE(menu->property("opened").toBool());
    // The focus is back on the table, and the menu key on the focused cell opens the same menu.
    CHECK(f.item()->hasActiveFocus());
    f.scene.key(Qt::Key_Menu);
    f.settle();
    CHECK(menu->property("opened").toBool());
    CHECK(labels() == expected);
    f.scene.key(Qt::Key_Escape);
}

// Written by reading: this tree's Qt Quick build was not configured, so these two cases were not compiled or run.
// Mutation: `setOnSelectAll` ignored, under which Ctrl+A reports the five keys through `onSelectionChange`.
TEST_CASE("qt_quick table: select-all asks the handler and marks the view", "[qt_quick]") {
    Scene scene;
    std::vector<ui::Key> keys;
    for (std::int64_t id = 1; id <= 5; ++id) {
        keys.emplace_back(id);
    }
    Rows table = makeTable(scene, keys, ui::SelectionMode::Multiple);
    table.table->setSelectColumn(true);
    scene.settle();
    int asked = 0;
    std::vector<std::pair<std::vector<ui::Key>, ui::SelectionChange>> reported;
    table.table->setOnSelectAll([&asked] { ++asked; });
    table.table->setOnSelectionChange([&reported](std::vector<ui::Key> chosen, ui::SelectionChange change) {
        reported.emplace_back(std::move(chosen), change);
    });
    scene.key(Qt::Key_Tab);
    scene.key(Qt::Key_A, Qt::ControlModifier);
    CHECK(asked == 1);
    scene.click(selectAllOf(*table.table));
    CHECK(asked == 2);
    CHECK(reported.empty());

    table.table->setSelectionView({ui::Key{std::int64_t{2}}});
    scene.settle();
    for (std::size_t at = 0; at < table.rows.size(); ++at) {
        CHECK(Scene::itemOf(*table.rows.at(at)).property("selected").toBool() == (at != 1));
    }
    scene.click(Scene::itemOf(*table.rows.at(0)), Qt::ControlModifier);
    REQUIRE(reported.size() == 1);
    CHECK(reported.back().second == ui::SelectionChange::Extend);
    CHECK(reported.back().first ==
          std::vector<ui::Key>{ui::Key{std::int64_t{3}}, ui::Key{std::int64_t{4}}, ui::Key{std::int64_t{5}}});
    table.table->setSelection({});
    scene.settle();
    for (auto const& row : table.rows) {
        CHECK_FALSE(Scene::itemOf(*row).property("selected").toBool());
    }
}

// Mutation: the entries drawn as plain text, under which the click reports nothing.
TEST_CASE("qt_quick table: a status entry reports its id", "[qt_quick]") {
    Scene scene;
    Rows table = makeTable(scene, {ui::Key{std::int64_t{1}}}, ui::SelectionMode::Multiple);
    std::vector<std::string> chosen;
    table.table->setOnStatusEntry([&chosen](std::string id) { chosen.push_back(std::move(id)); });
    table.table->setStatus(ui::TableStatus{
        .message = "m",
        .entries = {{.id = "run", .label = "Run on these"}},
        .standing = "1 row could not be saved",
        .standingEntries = {{.id = "select", .label = "Select them"}, {.id = "clear", .label = "Clear marks"}},
        .shown = true});
    scene.settle();
    QQuickItem& item = Scene::itemOf(*table.table);
    for (auto const& [id, label] :
         std::vector<std::pair<QString, QString>>{{QStringLiteral("run"), QStringLiteral("Run on these")},
                                                  {QStringLiteral("select"), QStringLiteral("Select them")},
                                                  {QStringLiteral("clear"), QStringLiteral("Clear marks")}}) {
        auto const* const button = itemNamed(item, QStringLiteral("statusEntry_") + id);
        REQUIRE(button != nullptr);
        CHECK(button->property("text").toString() == label);
    }
    scene.click(*itemNamed(item, QStringLiteral("statusEntry_clear")));
    CHECK(chosen == std::vector<std::string>{"clear"});
}

namespace {

// A grid of two specimens (`morph::test::grid::kSpecimens`) between two text inputs, `before` and `after`, on Qt
// Quick. Its columns are every member in schema order: label 0, mass 1, volume 2, density 3, limit 4, note 5.
struct QuickGrid {
    QuickGrid() {
        form = std::make_unique<morph::forms::FormSession>(
            scene.runtime(), morph::test::formsengine::modelOf("Lims_RecordSpecimens", morph::test::grid::kSpecimens),
            server.submitter(), server.fetcher(),
            morph::forms::FormSessionOptions{.locale = ui::localeFacts("de").numeric, .bcp47 = "de"});
        form->prefill(R"({"sampleId":1,"specimens":[{},{}]})");
        std::vector<ui::GridColumn> columns;
        for (morph::forms::FieldSpec const& member : form->field("specimens").spec().children) {
            columns.push_back(
                ui::GridColumn{.id = member.name, .label = member.name, .field = member.name, .value = {}});
        }
        ui::TableOptions table;
        table.common.testId = "grid";
        grid = ui::makeFormGrid(*form, form->field("specimens"), std::move(columns), {}, ui::formatOf(*form),
                                std::move(table));
        ui::TextInput before;
        before.common.testId = "before";
        ui::TextInput after;
        after.common.testId = "after";
        view.emplace(scene.runtime(), scene.backend(),
                     ui::column(ui::Column{
                         .children = {ui::textInput(std::move(before)), grid.node, ui::textInput(std::move(after))}}));
        scene.settle();
    }
    QuickGrid(QuickGrid const&) = delete;
    QuickGrid& operator=(QuickGrid const&) = delete;
    QuickGrid(QuickGrid&&) = delete;
    QuickGrid& operator=(QuickGrid&&) = delete;
    ~QuickGrid() {
        view.reset();
        grid = {};
    }

    [[nodiscard]] QQuickItem& named(QString const& name) {
        QQuickItem* const found = itemNamed(*scene.window().contentItem(), name);
        REQUIRE(found != nullptr);
        return *found;
    }

    // The row item of view row @p viewRow.
    [[nodiscard]] QQuickItem& rowAt(int viewRow) {
        for (QQuickItem* const row : Scene::itemsOf(named(QStringLiteral("grid")), "QQuickItem")) {
            if (row->property("viewRow").toInt() == viewRow && row->property("cellMarks").isValid()) {
                return *row;
            }
        }
        FAIL("no row at " << viewRow);
        throw std::logic_error{"unreachable"};
    }

    // The cell of view row @p viewRow in column @p column.
    [[nodiscard]] QQuickItem& cellAt(int viewRow, int column) {
        for (QQuickItem* const row : Scene::itemsOf(named(QStringLiteral("grid")), "QQuickItem")) {
            if (row->property("viewRow").toInt() != viewRow || !row->property("cellMarks").isValid()) {
                continue;
            }
            for (QQuickItem* const cell : Scene::itemsOf(*row, "QQuickItem")) {
                if (cell->property("measuredIndex").isValid() && cell->property("measuredIndex").toInt() == column &&
                    cell->isVisible()) {
                    return *cell;
                }
            }
        }
        FAIL("no cell at " << viewRow << ", " << column);
        throw std::logic_error{"unreachable"};
    }

    Scene scene;
    morph::test::formsengine::FakeServer server{scene.owner()};
    std::unique_ptr<morph::forms::FormSession> form;
    ui::FormGrid grid;
    std::optional<ui::Mounted> view;
};

}  // namespace

// Mutation: the QML cell ignoring setOnText.
TEST_CASE("qt_quick grid: typing on a focused cell opens its editor, Tab walks and leaves", "[qt_quick][grid]") {
    QuickGrid g;
    g.grid.grid->focusCell(morph::test::grid::at(1, "mass"));
    g.scene.settle();
    QQuickItem& table = g.named(QStringLiteral("grid"));
    table.forceActiveFocus();
    g.scene.type(QStringLiteral("5"));
    CHECK(g.grid.grid->editing() == morph::test::grid::at(1, "mass"));
    QList<QQuickItem*> const fields = Scene::itemsOf(table, "QQuickTextField");
    REQUIRE(fields.size() == 1);
    CHECK(fields.front()->property("text").toString() == QStringLiteral("5"));
    g.scene.key(Qt::Key_Tab);
    CHECK(g.form->field("specimens[0].mass").draft() == "5");
    CHECK(g.grid.grid->focus() == morph::test::grid::at(1, "volume"));
    g.grid.grid->focusCell(morph::test::grid::at(2, "note"));
    g.scene.settle();
    table.forceActiveFocus();
    g.scene.key(Qt::Key_Tab);
    QQuickItem const* const focused = g.scene.window().activeFocusItem();
    REQUIRE(focused != nullptr);
    CHECK(focused->objectName() == QStringLiteral("after"));
}

// Mutation: the QML cell sending a double click as two single clicks (no editor opens).
TEST_CASE("qt_quick grid: a double click opens the editor in the cell and focus leaving commits", "[qt_quick][grid]") {
    QuickGrid g;
    QQuickItem const& cell = g.cellAt(1, 1);
    // The cell's slot: its column's span across the row's height. A cell draws only as tall as its text, which a wide
    // font leaves shorter than the editor that replaces it.
    QRectF const area =
        cell.parentItem()->mapRectToScene(QRectF{cell.x(), 0, cell.width(), cell.parentItem()->height()});
    g.scene.doubleClick(cell);
    CHECK(g.grid.grid->editing() == morph::test::grid::at(2, "mass"));
    QList<QQuickItem*> const fields = Scene::itemsOf(g.named(QStringLiteral("grid")), "QQuickTextField");
    REQUIRE(fields.size() == 1);
    QPointF const centre =
        fields.front()->mapToScene(QPointF{fields.front()->width() / 2, fields.front()->height() / 2});
    CHECK(area.contains(centre));
    CHECK(fields.front()->hasActiveFocus());
    g.scene.type(QStringLiteral("7"));
    g.scene.click(g.named(QStringLiteral("after")));
    CHECK(g.form->field("specimens[1].mass").draft() == "7");
    CHECK(Scene::itemsOf(g.named(QStringLiteral("grid")), "QQuickTextField").empty());
}

// Mutation: the column's field resolved once for every row (row 2's editor is a ComboBox too).
TEST_CASE("qt_quick grid: a column's kind follows its row", "[qt_quick][grid]") {
    Scene scene;
    morph::test::formsengine::FakeServer server{scene.owner()};
    auto form = std::make_unique<morph::forms::FormSession>(
        scene.runtime(), morph::test::formsengine::modelOf("Lims_RecordRows", morph::test::grid::kKindPerRow),
        server.submitter(), server.fetcher(),
        morph::forms::FormSessionOptions{.locale = ui::localeFacts("de").numeric, .bcp47 = "de"});
    form->prefill(R"({"rows":[{"isChoice":true},{"isChoice":false}]})");
    ui::TableOptions options;
    options.common.testId = "grid";
    ui::FormGrid grid = ui::makeFormGrid(*form, form->field("rows"), morph::test::grid::kindPerRowColumns(), {},
                                         ui::formatOf(*form), std::move(options));
    std::optional<ui::Mounted> view;
    view.emplace(scene.runtime(), scene.backend(), grid.node);
    scene.settle();
    QQuickItem* const table = itemNamed(*scene.window().contentItem(), QStringLiteral("grid"));
    REQUIRE(table != nullptr);
    grid.grid->activate(morph::test::grid::at(1, "value"));
    scene.settle();
    CHECK(Scene::itemsOf(*table, "QQuickComboBox").size() == 1);
    CHECK(Scene::itemsOf(*table, "QQuickTextField").empty());
    grid.grid->press("Escape");
    grid.grid->activate(morph::test::grid::at(2, "value"));
    scene.settle();
    CHECK(Scene::itemsOf(*table, "QQuickComboBox").empty());
    CHECK(Scene::itemsOf(*table, "QQuickTextField").size() == 1);
    view.reset();
    grid = {};
}

// Mutation: the QML table reporting the drag as a row selection (the rectangle is not selected and a row is).
TEST_CASE("qt_quick grid: a drag selects a rectangle of cells and a row header click its row", "[qt_quick][grid]") {
    QuickGrid g;
    QQuickItem& table = g.named(QStringLiteral("grid"));
    CHECK(table.property("selectsCells").toBool());
    QPoint const start = Scene::centreOf(g.cellAt(0, 1));  // (1, mass)
    QPoint const end = Scene::centreOf(g.cellAt(1, 2));    // (2, volume)
    QTest::mousePress(&g.scene.window(), Qt::LeftButton, {}, start);
    constexpr int steps = 10;
    for (int step = 1; step <= steps; ++step) {
        QTest::mouseMove(&g.scene.window(), start + ((end - start) * step / steps));
    }
    QTest::mouseRelease(&g.scene.window(), Qt::LeftButton, {}, end);
    g.scene.settle();
    CHECK(g.grid.grid->selection() ==
          ui::CellRange{morph::test::grid::at(1, "mass"), morph::test::grid::at(2, "volume")});
    QVariantList const rect = table.property("cellRect").toList();
    REQUIRE(rect.size() == 4);
    CHECK(rect.at(0).toInt() == 0);
    CHECK(rect.at(1).toInt() == 2);
    CHECK(rect.at(2).toInt() == 1);
    CHECK(rect.at(3).toInt() == 2);
    QQuickItem const& box = g.named(QStringLiteral("cellSelection"));
    CHECK(box.isVisible());
    for (QQuickItem const* const row : Scene::itemsOf(table, "QQuickItem")) {
        if (row->property("cellMarks").isValid()) {
            CHECK_FALSE(row->property("selected").toBool());
        }
    }
    // A click on a row's header, the leading strip before its first cell, selects the row's cells.
    qreal const lead = table.property("lead").toReal();
    REQUIRE(lead > 0);
    QQuickItem const& row = g.rowAt(1);
    QPointF const header = row.mapToScene(QPointF{lead / 2, row.height() / 2});
    QTest::mouseClick(&g.scene.window(), Qt::LeftButton, {}, header.toPoint());
    g.scene.settle();
    CHECK(g.grid.grid->selection() ==
          ui::CellRange{morph::test::grid::at(2, "label"), morph::test::grid::at(2, "note")});
}

// Mutation: the header's sort handler taking only a Shift-click in a table that selects cells (the plain click reports
// no sort).
TEST_CASE("qt_quick grid: a header click sorts, a Shift-click adds, a Ctrl-click selects the column",
          "[qt_quick][grid]") {
    Scene scene;
    morph::test::formsengine::FakeServer server{scene.owner()};
    auto form = std::make_unique<morph::forms::FormSession>(
        scene.runtime(), morph::test::formsengine::modelOf("Lims_RecordSpecimens", morph::test::grid::kSpecimens),
        server.submitter(), server.fetcher(),
        morph::forms::FormSessionOptions{.locale = ui::localeFacts("de").numeric, .bcp47 = "de"});
    form->prefill(R"({"sampleId":1,"specimens":[{},{}]})");
    std::vector<ui::GridColumn> columns;
    std::vector<ui::TableColumn> shown;
    for (morph::forms::FieldSpec const& member : form->field("specimens").spec().children) {
        columns.push_back(ui::GridColumn{.id = member.name, .label = member.name, .field = member.name, .value = {}});
        shown.push_back(ui::TableColumn{.id = member.name, .label = member.name, .sortable = member.name == "mass"});
    }
    std::vector<std::pair<std::size_t, ui::HeaderClick>> sorts;
    ui::TableOptions options;
    options.common.testId = "grid";
    options.onSort = [&sorts](std::size_t column, ui::HeaderClick click) { sorts.emplace_back(column, click); };
    ui::FormGrid grid = ui::makeFormGrid(*form, form->field("specimens"), std::move(columns), {}, ui::formatOf(*form),
                                         std::move(options), std::move(shown));
    std::optional<ui::Mounted> view;
    view.emplace(scene.runtime(), scene.backend(), grid.node);
    scene.settle();
    QQuickItem* const table = itemNamed(*scene.window().contentItem(), QStringLiteral("grid"));
    REQUIRE(table != nullptr);
    QQuickItem const* const header = itemNamed(*table, QStringLiteral("header1"));  // mass
    REQUIRE(header != nullptr);
    scene.click(*header);
    REQUIRE(sorts.size() == 1);
    CHECK(sorts.back() == std::pair{std::size_t{1}, ui::HeaderClick::Plain});
    scene.click(*header, Qt::ShiftModifier);
    REQUIRE(sorts.size() == 2);
    CHECK(sorts.back() == std::pair{std::size_t{1}, ui::HeaderClick::Shift});
    scene.click(*header, Qt::ControlModifier);
    CHECK(sorts.size() == 2);  // a Ctrl-click sorts nothing
    CHECK(grid.grid->selection() == ui::CellRange{morph::test::grid::at(1, "mass"), morph::test::grid::at(2, "mass")});
    view.reset();
    grid = {};
}

// Mutation: the QML band item placed from its first run's x to its last run's end (one item, as wide as columns 1 to
// 4).
TEST_CASE("qt_quick table: a band draws over each run of its columns", "[qt_quick][bands]") {
    Scene scene;
    Rows table = makeTable(scene, {ui::Key{std::int64_t{1}}}, ui::SelectionMode::None);
    table.table->setColumns({{.label = "Name", .width = ui::Sizing::fixed(8)},
                             {.label = "M8", .width = ui::Sizing::fixed(8)},
                             {.label = "M2", .width = ui::Sizing::fixed(8)},
                             {.label = "Total", .width = ui::Sizing::fixed(8)},
                             {.label = "M4", .width = ui::Sizing::fixed(8)}});
    table.table->setBands(
        {{.level = 0, .label = "Mass retained", .unit = "g", .first = 1, .count = 2, .pinned = false},
         {.level = 0, .label = "Mass retained", .unit = "g", .first = 4, .count = 1, .pinned = false}});
    scene.settle();
    QQuickItem& tableItem = Scene::itemOf(*table.table);
    auto* const first = itemNamed(tableItem, QStringLiteral("band0"));
    auto* const second = itemNamed(tableItem, QStringLiteral("band1"));
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);
    QQuickItem const& m8 = headerOf(*table.table, 1);
    QQuickItem const& m2 = headerOf(*table.table, 2);
    QQuickItem const& m4 = headerOf(*table.table, 4);
    auto const left = [&first](QQuickItem const& item) { return item.mapToItem(first->parentItem(), QPointF{}).x(); };
    CHECK(first->x() == Catch::Approx(left(m8)));
    CHECK(first->x() + first->width() == Catch::Approx(left(m2) + m2.width()));
    CHECK(second->x() == Catch::Approx(left(m4)));
    CHECK(second->width() == Catch::Approx(m4.width()));
}

// Mutation: `setRowSummary` writing nothing (the row's summary surface stays hidden).
TEST_CASE("qt_quick table: a summary row is drawn on the summary surface", "[qt_quick][summary]") {
    Scene scene;
    Rows table = makeTable(scene, {ui::Key{std::int64_t{1}}, ui::Key{std::int64_t{2}}}, ui::SelectionMode::Single);
    table.table->setRowSummary(*table.rows.at(1));
    scene.settle();
    auto const fill = [](ui::Widget& row) {
        return Scene::itemOf(row).findChild<QQuickItem*>(QStringLiteral("summaryFill"));
    };
    REQUIRE(fill(*table.rows.at(1)) != nullptr);
    CHECK(fill(*table.rows.at(1))->isVisible());
    CHECK_FALSE(fill(*table.rows.at(0))->isVisible());
}

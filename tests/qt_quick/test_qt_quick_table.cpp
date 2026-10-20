// SPDX-License-Identifier: Apache-2.0
//
// The table on the real Qt Quick engine: cells placed by column, selection by key in each mode, activation without
// toggling, and keys that cross into QML as text, an int64 above 2^53 unchanged.

#include <QColor>
#include <QObject>
#include <QQuickItem>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <memory>
#include <morph/ui/backend.hpp>
#include <optional>
#include <string>
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
    table.table->setOnSelectionChange([&reported](std::vector<ui::Key> keys) { reported.push_back(std::move(keys)); });
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
    table.table->setOnSelectionChange([&reports](std::vector<ui::Key> const&) { ++reports; });
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

// SPDX-License-Identifier: Apache-2.0
//
// The containers on the real Qt Quick engine: children in order, moves that keep the moved item, and what each
// container kind shows and reports.

#include <QPointF>
#include <QQuickItem>
#include <QStringList>
#include <QVariantList>
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <memory>
#include <morph/qt_quick/theme.hpp>
#include <morph/ui/backend.hpp>
#include <morph/ui/theme.hpp>
#include <string>
#include <vector>

#include "qt_quick/kinds.hpp"
#include "qt_quick_harness.hpp"

namespace ui = morph::ui;
namespace detail = morph::qt_quick::detail;
using morph::qt_quick::testing::Scene;

namespace {

detail::ContainerCore& containerCore(ui::ContainerWidget& container) {
    auto* const found = dynamic_cast<detail::ContainerCore*>(&container);
    REQUIRE(found != nullptr);
    return *found;
}

// The texts a container's children show, top to bottom as laid out.
std::vector<std::string> laidOut(Scene& scene, std::vector<std::unique_ptr<ui::TextWidget>> const& texts) {
    scene.settle();
    std::vector<std::pair<qreal, std::string>> placed;
    placed.reserve(texts.size());
    for (auto const& text : texts) {
        QQuickItem const& item = Scene::itemOf(*text);
        placed.emplace_back(item.mapToScene(QPointF{0, 0}).y(), detail::baseOf(*text)->shownText());
    }
    std::ranges::sort(placed);
    std::vector<std::string> order;
    order.reserve(placed.size());
    for (auto const& entry : placed) {
        order.push_back(entry.second);
    }
    return order;
}

}  // namespace

TEST_CASE(
    "qt_quick containers: a move puts the child before the child now at the index, as one restack, and keeps "
    "its item",
    "[qt_quick]") {
    Scene scene;
    auto column = scene.backend().createStack(nullptr, ui::Axis::Vertical);
    column->setGap(1);
    std::vector<std::unique_ptr<ui::TextWidget>> texts;
    for (char const* const name : {"a", "b", "c"}) {
        texts.push_back(scene.backend().createText(column.get()));
        texts.back()->setText(name);
    }
    QQuickItem const* const movedItem = &Scene::itemOf(*texts.at(2));
    CHECK(laidOut(scene, texts) == std::vector<std::string>{"a", "b", "c"});
    column->moveChild(*texts.at(2), 0);
    CHECK(laidOut(scene, texts) == std::vector<std::string>{"c", "a", "b"});
    CHECK(&Scene::itemOf(*texts.at(2)) == movedItem);
    CHECK(containerCore(*column).restacks() == 1);
    column->moveChild(*texts.at(2), 99);
    CHECK(laidOut(scene, texts) == std::vector<std::string>{"a", "b", "c"});
    // A move to where the child already is changes nothing.
    column->moveChild(*texts.at(2), 2);
    CHECK(containerCore(*column).restacks() == 2);
    texts.erase(texts.begin() + 1);
    CHECK(containerCore(*column).children().size() == 2);
    CHECK(laidOut(scene, texts) == std::vector<std::string>{"a", "c"});
}

TEST_CASE("qt_quick containers: a grid lays children in its columns and widens a spanned one", "[qt_quick]") {
    Scene scene;
    auto grid = scene.backend().createGrid(nullptr);
    grid->setColumns(2);
    grid->setGap(1);
    auto first = scene.backend().createText(grid.get());
    auto second = scene.backend().createText(grid.get());
    auto wide = scene.backend().createText(grid.get());
    first->setText("1");
    second->setText("2");
    wide->setText("3");
    grid->setSpan(*wide, 2);
    scene.settle();
    CHECK(Scene::itemOf(*grid).property("columns").toInt() == 2);
    CHECK(Scene::itemOf(*wide).property("columnSpan").toInt() == 2);
    QQuickItem const& one = Scene::itemOf(*first);
    QQuickItem const& two = Scene::itemOf(*second);
    QQuickItem const& three = Scene::itemOf(*wide);
    CHECK(one.y() == two.y());
    CHECK(two.x() > one.x());
    CHECK(three.y() > one.y());
    grid->setColumns(0);
    CHECK(Scene::itemOf(*grid).property("columns").toInt() == 1);
}

TEST_CASE("qt_quick containers: a collapsible panel reports the state the user asks for and hides its content",
          "[qt_quick]") {
    Scene scene;
    auto panel = scene.backend().createPanel(nullptr);
    panel->setTitle("Details");
    panel->setPadding(1);
    panel->setCollapsible(true);
    std::vector<bool> toggles;
    panel->setOnToggle([&toggles](bool collapsed) { toggles.push_back(collapsed); });
    auto inside = scene.backend().createButton(panel.get());
    scene.settle();
    CHECK(Scene::itemOf(*inside).isVisible());
    QList<QQuickItem*> const buttons = Scene::itemsOf(Scene::itemOf(*panel), "QQuickToolButton");
    REQUIRE(buttons.size() == 1);
    scene.click(*buttons.at(0));
    CHECK(toggles == std::vector<bool>{true});
    CHECK_FALSE(Scene::itemOf(*inside).isVisible());
    panel->setCollapsed(false);
    scene.settle();
    CHECK(Scene::itemOf(*inside).isVisible());
    CHECK(toggles.size() == 1);
}

TEST_CASE("qt_quick containers: tabs show their labels, highlight on request and report a click", "[qt_quick]") {
    Scene scene;
    auto tabs = scene.backend().createTabs(nullptr);
    tabs->setTabs({"One", "Two"});
    std::vector<std::size_t> picks;
    tabs->setOnSelect([&picks](std::size_t index) { picks.push_back(index); });
    auto page = scene.backend().createSlot(tabs.get());
    scene.settle();
    auto& impl = dynamic_cast<detail::TabsImpl&>(*tabs);
    tabs->setSelected(1);
    CHECK(impl.highlighted() == 1);
    tabs->setSelected(5);
    CHECK(impl.highlighted() == -1);
    CHECK(picks.empty());
    QList<QQuickItem*> const buttons = Scene::itemsOf(Scene::itemOf(*tabs), "QQuickTabButton");
    REQUIRE(buttons.size() == 2);
    scene.click(*buttons.at(0));
    CHECK(picks == std::vector<std::size_t>{0});
    CHECK(impl.highlighted() == 0);
    CHECK(containerCore(*tabs).children().size() == 1);
}

TEST_CASE("qt_quick containers: a splitter takes its panes through the split view and applies sizes", "[qt_quick]") {
    Scene scene;
    auto splitter = scene.backend().createSplitter(nullptr, ui::Axis::Horizontal);
    splitter->setLayout({.width = ui::Sizing::stretch(), .height = ui::Sizing::fixed(10)});
    auto left = scene.backend().createText(splitter.get());
    auto right = scene.backend().createText(splitter.get());
    left->setText("left");
    right->setText("right");
    std::vector<std::vector<int>> resized;
    splitter->setOnResize([&resized](std::vector<int> sizes) { resized.push_back(std::move(sizes)); });
    splitter->setSizes({10, 30});
    scene.settle();
    QQuickItem const& leftItem = Scene::itemOf(*left);
    QQuickItem const& rightItem = Scene::itemOf(*right);
    CHECK(rightItem.x() > leftItem.x());
    CHECK(rightItem.width() > leftItem.width());
    splitter->moveChild(*right, 0);
    scene.settle();
    CHECK(Scene::itemOf(*right).x() < Scene::itemOf(*left).x());
    // Sizes that do not match the pane count share the space equally.
    splitter->setSizes({});
    scene.settle();
    // Within the handle's width, which the split view takes from one pane.
    CHECK(std::abs(Scene::itemOf(*left).width() - Scene::itemOf(*right).width()) <= 8.0);
    right.reset();
    scene.settle();
    CHECK(containerCore(*splitter).children().size() == 1);
    CHECK(resized.empty());
}

TEST_CASE("qt_quick containers: a collapsible section keeps its header usable while closed", "[qt_quick]") {
    Scene scene;
    auto section = scene.backend().createCollapsible(nullptr);
    section->setTitle("More");
    auto header = scene.backend().createButton(section.get());
    section->setHeader(*header);
    auto content = scene.backend().createButton(section.get());
    std::vector<bool> toggles;
    section->setOnToggle([&toggles](bool open) { toggles.push_back(open); });
    int headerClicks = 0;
    header->setOnClick([&headerClicks] { ++headerClicks; });
    section->setOpen(false);
    scene.settle();
    CHECK_FALSE(Scene::itemOf(*content).isVisible());
    CHECK(Scene::itemOf(*header).isVisible());
    scene.click(Scene::itemOf(*header));
    CHECK(headerClicks == 1);
    dynamic_cast<detail::CollapsibleImpl&>(*section).toggleByUser();
    scene.settle();
    CHECK(toggles == std::vector<bool>{true});
    CHECK(Scene::itemOf(*content).isVisible());
}

TEST_CASE("qt_quick containers: a drop zone takes a drop whole or refuses it whole", "[qt_quick]") {
    Scene scene;
    auto zone = scene.backend().createDropZone(nullptr);
    zone->setAccept({".CSV", ".tsv"});
    zone->setMultiple(false);
    std::vector<std::vector<std::string>> drops;
    zone->setOnDrop([&drops](std::vector<std::string> paths) { drops.push_back(std::move(paths)); });
    auto inside = scene.backend().createText(zone.get());
    inside->setText("Drop here");
    scene.settle();
    auto& impl = dynamic_cast<detail::DropZoneImpl&>(*zone);
    CHECK(impl.offer({"/tmp/a.csv"}));
    CHECK_FALSE(impl.offer({"/tmp/a.txt"}));
    CHECK_FALSE(impl.offer({"/tmp/a.csv", "/tmp/b.csv"}));
    CHECK_FALSE(impl.offer({}));
    zone->setMultiple(true);
    CHECK(impl.offer({"/tmp/a.csv", "/tmp/b.TSV"}));
    CHECK(drops == std::vector<std::vector<std::string>>{{"/tmp/a.csv"}, {"/tmp/a.csv", "/tmp/b.TSV"}});
    zone->setAccept({});
    CHECK(impl.takes({"/tmp/anything"}));
    zone->setVisible(false);
    scene.settle();
    CHECK_FALSE(impl.offer({"/tmp/a.csv"}));
}

TEST_CASE("qt_quick containers: a scroll area brings the focused child into view", "[qt_quick]") {
    Scene scene;
    auto scroll = scene.backend().createScroll(nullptr, ui::Axis::Vertical);
    scroll->setLayout({.width = ui::Sizing::stretch(), .height = ui::Sizing::fixed(5)});
    constexpr std::size_t count = 20;
    std::vector<std::unique_ptr<ui::TextInputWidget>> inputs;
    inputs.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        inputs.push_back(scene.backend().createTextInput(scroll.get(), ui::TextInputMode::SingleLine));
    }
    scene.settle();
    QQuickItem const& area = Scene::itemOf(*scroll);
    QQuickItem const& last = Scene::controlOf(*inputs.back());
    CHECK(last.mapToItem(&area, QPointF{0, 0}).y() > area.height());
    inputs.back()->focus();
    scene.settle();
    QPointF const shown = last.mapToItem(&area, QPointF{0, 0});
    CHECK(shown.y() >= 0);
    CHECK(shown.y() + last.height() <= area.height() + 1);
}

namespace {

/// The scene-space distance from the bottom of @p above to the top of @p below.
qreal verticalGap(QQuickItem const& above, QQuickItem const& below) {
    return below.mapToScene(QPointF{0, 0}).y() - (above.mapToScene(QPointF{0, 0}).y() + above.height());
}

/// The scene-space distance from the right of @p left to the left of @p right.
qreal horizontalGap(QQuickItem const& left, QQuickItem const& right) {
    return right.mapToScene(QPointF{0, 0}).x() - (left.mapToScene(QPointF{0, 0}).x() + left.width());
}

}  // namespace

TEST_CASE("qt_quick containers: a stack's gap is its spacing step's distance on the theme's scale", "[qt_quick]") {
    Scene scene;
    auto column = scene.backend().createStack(nullptr, ui::Axis::Vertical);
    column->setGap(2);
    auto title = scene.backend().createText(column.get());
    title->setText("Title");
    auto first = scene.backend().createTextInput(column.get(), ui::TextInputMode::SingleLine);
    auto second = scene.backend().createTextInput(column.get(), ui::TextInputMode::SingleLine);
    scene.settle();
    CHECK(verticalGap(Scene::itemOf(*title), Scene::itemOf(*first)) == 8.0);
    CHECK(verticalGap(Scene::itemOf(*first), Scene::itemOf(*second)) == 8.0);

    auto row = scene.backend().createStack(column.get(), ui::Axis::Horizontal);
    row->setGap(5);
    auto left = scene.backend().createButton(row.get());
    auto right = scene.backend().createButton(row.get());
    left->setLabel("Left");
    right->setLabel("Right");
    scene.settle();
    CHECK(horizontalGap(Scene::itemOf(*left), Scene::itemOf(*right)) == 24.0);
}

TEST_CASE("qt_quick containers: a grid's gap and a panel's padding are spacing steps", "[qt_quick]") {
    Scene scene;
    auto grid = scene.backend().createGrid(nullptr);
    grid->setColumns(2);
    grid->setGap(1);
    auto a = scene.backend().createText(grid.get());
    auto b = scene.backend().createText(grid.get());
    auto c = scene.backend().createText(grid.get());
    a->setText("a");
    b->setText("b");
    c->setText("c");
    auto panel = scene.backend().createPanel(nullptr);
    panel->setPadding(3);
    auto inside = scene.backend().createText(panel.get());
    inside->setText("inside");
    scene.settle();
    CHECK(verticalGap(Scene::itemOf(*a), Scene::itemOf(*c)) == 4.0);
    CHECK(Scene::itemOf(*b).mapToScene(QPointF{0, 0}).x() - Scene::itemOf(*a).mapToScene(QPointF{0, 0}).x() >=
          Scene::itemOf(*a).width() + 4.0);
    // The frame's one-pixel border, then the step's 12 px.
    CHECK(Scene::itemOf(*inside).mapToScene(QPointF{0, 0}).x() - Scene::itemOf(*panel).mapToScene(QPointF{0, 0}).x() ==
          13.0);
}

TEST_CASE("qt_quick containers: a theme's space token sets what a spacing step is", "[qt_quick]") {
    Scene scene;
    auto const tokens =
        ui::parseTokens(R"({"space": {"$type": "dimension", "2": {"$value": {"value": 20, "unit": "px"}}}})");
    REQUIRE(tokens);
    auto const resolved = ui::resolveTokens({&*tokens});
    REQUIRE(resolved);
    REQUIRE(morph::qt_quick::applyTheme(scene.engine(), ui::themeValues(*resolved)).empty());
    auto column = scene.backend().createStack(nullptr, ui::Axis::Vertical);
    column->setGap(2);
    auto first = scene.backend().createText(column.get());
    auto second = scene.backend().createText(column.get());
    first->setText("first");
    second->setText("second");
    scene.settle();
    CHECK(verticalGap(Scene::itemOf(*first), Scene::itemOf(*second)) == 20.0);
}

TEST_CASE("qt_quick containers: a content child fills its container across the axis", "[qt_quick]") {
    Scene scene;
    auto outer = scene.backend().createStack(nullptr, ui::Axis::Vertical);
    outer->setLayout({.width = ui::Sizing::fixed(40), .height = {}});
    auto column = scene.backend().createStack(outer.get(), ui::Axis::Vertical);
    auto input = scene.backend().createTextInput(column.get(), ui::TextInputMode::SingleLine);
    auto narrow = scene.backend().createButton(column.get());
    narrow->setLabel("Narrow");
    narrow->setLayout({.width = ui::Sizing::fixed(10), .height = {}});
    auto row = scene.backend().createStack(column.get(), ui::Axis::Horizontal);
    auto tall = scene.backend().createText(row.get());
    tall->setText("one\ntwo\nthree");
    auto button = scene.backend().createButton(row.get());
    button->setLabel("Go");
    auto other = scene.backend().createButton(row.get());
    other->setLabel("Stop");
    scene.settle();

    qreal const unit = Scene::itemOf(*outer).width() / 40.0;
    // In a column, a content child is as wide as the column; a fixed one keeps its width.
    CHECK(std::abs(Scene::itemOf(*input).width() - Scene::itemOf(*outer).width()) < 1.0);
    CHECK(Scene::itemOf(*narrow).width() == 10.0 * unit);
    // A button keeps its natural width at the start of the line, as the terminal draws it.
    auto label = scene.backend().createButton(column.get());
    label->setLabel("Go on");
    scene.settle();
    CHECK(Scene::itemOf(*label).width() == Scene::itemOf(*label).implicitWidth());
    CHECK(Scene::itemOf(*label).mapToScene(QPointF{0, 0}).x() == Scene::itemOf(*column).mapToScene(QPointF{0, 0}).x());
    // In a row, a content child is as tall as the row, and keeps its natural width.
    CHECK(Scene::itemOf(*button).height() == Scene::itemOf(*row).height());
    CHECK(Scene::itemOf(*button).height() > Scene::itemOf(*other).implicitHeight());
    CHECK(Scene::itemOf(*button).width() == Scene::itemOf(*button).implicitWidth());
    // The row packs its children at the start rather than spreading them.
    CHECK(horizontalGap(Scene::itemOf(*button), Scene::itemOf(*other)) == 0.0);
}

TEST_CASE("qt_quick containers: a content-sized splitter is as large as its panes", "[qt_quick]") {
    Scene scene;
    auto row = scene.backend().createStack(nullptr, ui::Axis::Horizontal);
    auto splitter = scene.backend().createSplitter(row.get(), ui::Axis::Horizontal);
    auto left = scene.backend().createText(splitter.get());
    auto right = scene.backend().createText(splitter.get());
    left->setText("Left");
    right->setText("Right");
    scene.settle();
    QQuickItem const& leftItem = Scene::itemOf(*left);
    QQuickItem const& rightItem = Scene::itemOf(*right);
    CHECK(Scene::itemOf(*splitter).width() >= leftItem.implicitWidth() + rightItem.implicitWidth());
    CHECK(Scene::itemOf(*splitter).height() >= leftItem.implicitHeight());
    CHECK(rightItem.mapToScene(QPointF{0, 0}).x() >=
          leftItem.mapToScene(QPointF{0, 0}).x() + leftItem.implicitWidth());

    splitter->setSizes({10, 30});
    scene.settle();
    qreal const unit = Scene::itemOf(*splitter).property("unit").toReal();
    CHECK(Scene::itemOf(*splitter).implicitWidth() >= 40.0 * unit);
    CHECK(Scene::itemOf(*splitter).implicitWidth() < 40.0 * unit + 16.0);
}

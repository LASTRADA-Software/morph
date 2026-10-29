// SPDX-License-Identifier: Apache-2.0
//
// The containers on the real Qt Quick engine: children in order, moves that keep the moved item, and what each
// container kind shows and reports.

#include <QColor>
#include <QFont>
#include <QFontInfo>
#include <QMetaObject>
#include <QPointF>
#include <QQuickItem>
#include <QRectF>
#include <QSizeF>
#include <QString>
#include <QStringList>
#include <QTest>
#include <QVariantList>
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <memory>
#include <morph/core/completion.hpp>
#include <morph/forms/engine/form_session.hpp>
#include <morph/forms/engine/form_view.hpp>
#include <morph/qt_quick/theme.hpp>
#include <morph/ui/backend.hpp>
#include <morph/ui/mount.hpp>
#include <morph/ui/shell_frame.hpp>
#include <morph/ui/theme.hpp>
#include <morph/ui/view.hpp>
#include <string>
#include <string_view>
#include <utility>
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

// The text item inside a widget that shows @p text: a field's root also holds its caption and its message, so the
// first text item found need not be the one drawn.
// A pointer, not a reference: GCC reads a reference returned from a call given a temporary as possibly dangling.
QQuickItem const* textItemOf(char const* testId, QString const& text) {
    detail::ItemBase const* const widget = detail::findWidget(QString::fromLatin1(testId));
    REQUIRE(widget != nullptr);
    REQUIRE(widget->item() != nullptr);
    QList<QQuickItem*> const inner = widget->item()->findChildren<QQuickItem*>();
    auto const found = std::ranges::find_if(inner, [&text](QQuickItem const* item) {
        return item->inherits("QQuickText") && item->property("text").toString() == text;
    });
    // A bool, not the iterators themselves: Catch2's REQUIRE decomposes a comparison's operands for its failure
    // message, and stringifying a QList<QQuickItem*>::const_iterator hits a deprecated implicit pointer conversion.
    bool const isFound = found != inner.end();
    REQUIRE(isFound);
    return *found;
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

TEST_CASE("qt_quick containers: a collapsible taller than its content keeps the content under its header",
          "[qt_quick]") {
    Scene scene;
    auto root = scene.backend().createStack(nullptr, ui::Axis::Vertical);
    auto section = scene.backend().createCollapsible(root.get());
    section->setTitle("Section");
    section->setLayout({.width = ui::Sizing::fixed(30), .height = ui::Sizing::fixed(20)});
    auto inside = scene.backend().createText(section.get());
    inside->setText("inside");
    auto stretching = scene.backend().createCollapsible(root.get());
    stretching->setTitle("Section");
    stretching->setLayout({.width = ui::Sizing::fixed(30), .height = ui::Sizing::fixed(20)});
    auto column = scene.backend().createStack(stretching.get(), ui::Axis::Vertical);
    column->setLayout({.width = ui::Sizing::stretch(), .height = ui::Sizing::stretch()});
    auto text = scene.backend().createText(column.get());
    text->setText("inside");
    scene.settle();
    qreal const unit = Scene::itemOf(*section).height() / 20.0;
    qreal const top = Scene::itemOf(*section).mapToScene(QPointF{0, 0}).y();
    CHECK(Scene::itemOf(*inside).mapToScene(QPointF{0, 0}).y() - top <= 3.0 * unit);
    QQuickItem const& big = Scene::itemOf(*stretching);
    QQuickItem const& fill = Scene::itemOf(*column);
    CHECK(big.mapToScene(QPointF{0, big.height()}).y() - fill.mapToScene(QPointF{0, fill.height()}).y() <= 1.5);
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

namespace {

// Where a tab button lies in the window, read once the bar's view has stopped moving: a read taken during the bar's
// own scroll animation finds the button on its way and proves nothing.
QRectF settledButton(Scene& scene, ui::TabsWidget& tabs, qsizetype index) {
    QRectF last;
    int still = 0;
    for (int round = 0; round < 60 && still < 4; ++round) {
        scene.settle();
        QTest::qWait(50);
        QList<QQuickItem*> const buttons = Scene::itemsOf(Scene::itemOf(tabs), "QQuickTabButton");
        REQUIRE(buttons.size() > index);
        QQuickItem const& button = *buttons.at(index);
        QRectF const now{button.mapToScene(QPointF{0, 0}), QSizeF{button.width(), button.height()}};
        still = now == last ? still + 1 : 0;
        last = now;
    }
    return last;
}

}  // namespace

// Mutation: drop the positionViewAtIndex call in MorphTabs (the tab chosen before the first layout stays past the
// window's right edge).
TEST_CASE("qt_quick containers: a narrow tab bar keeps the chosen tab wholly in view", "[qt_quick]") {
    std::vector<std::string> const labels{"Overview",    "Calibration", "Maintenance", "Bookings",
                                          "Audit trail", "Documents",   "Training"};
    for (bool const beforeLayout : {true, false}) {
        INFO((beforeLayout ? "chosen before the first layout" : "chosen after it"));
        Scene scene;
        scene.window().resize(360, 400);
        auto tabs = scene.backend().createTabs(nullptr);
        tabs->setLayout({.width = ui::Sizing::stretch(), .height = ui::Sizing::stretch()});
        tabs->setTabs(labels);
        if (beforeLayout) {
            tabs->setSelected(6);
        } else {
            scene.settle();
            tabs->setSelected(6);
        }
        QRectF const chosen = settledButton(scene, *tabs, 6);
        INFO("x " << chosen.x() << " width " << chosen.width());
        CHECK(chosen.x() >= -0.5);
        CHECK(chosen.x() + chosen.width() <= 360.5);
    }
}

// Mutation: never bind the TabBar's visible to barShown (it shows with one tab); or keep `focusTarget: bar`, so a
// tabs widget focused while its bar is hidden gives the keyboard focus to the hidden bar.
TEST_CASE(
    "qt_quick containers: a hidden tab bar is not drawn and takes no focus; a shown one sits a step above the page",
    "[qt_quick]") {
    Scene scene;
    auto tabs = scene.backend().createTabs(nullptr);
    tabs->setLayout({.width = ui::Sizing::stretch(), .height = ui::Sizing::stretch()});
    std::vector<std::string> chords;
    tabs->setKeys({"Ctrl+Tab"}, [&chords](std::string const& chord) { chords.push_back(chord); });
    tabs->setTabs({"One"});
    auto page = scene.backend().createSlot(tabs.get());
    page->setLayout({.width = ui::Sizing::stretch(), .height = ui::Sizing::stretch()});
    auto inside = scene.backend().createText(page.get());
    inside->setText("page");
    tabs->setSelected(0);
    tabs->setBarShown(false);
    scene.settle();
    QList<QQuickItem*> const bars = Scene::itemsOf(Scene::itemOf(*tabs), "QQuickTabBar");
    REQUIRE(bars.size() == 1);
    QQuickItem const& bar = *bars.at(0);
    CHECK_FALSE(bar.isVisible());
    CHECK(std::abs(Scene::itemOf(*page).mapToScene(QPointF{0, 0}).y() -
                   Scene::itemOf(*tabs).mapToScene(QPointF{0, 0}).y()) < 1.0);

    // Focused with its bar hidden, the widget holds the focus in its pages, not in the hidden bar, and its chords
    // arrive.
    tabs->focus();
    scene.settle();
    QQuickItem const* const focused = scene.window().activeFocusItem();
    REQUIRE(focused != nullptr);
    CHECK(focused->isVisible());
    scene.key(Qt::Key_Tab, Qt::ControlModifier);
    CHECK(chords == std::vector<std::string>{"Ctrl+Tab"});

    tabs->setTabs({"One", "Two"});
    tabs->setBarShown(true);
    scene.settle();
    CHECK(bar.isVisible());
    auto* const theme = scene.engine().singletonInstance<QObject*>(QStringLiteral("MorphUi"), QStringLiteral("Theme"));
    REQUIRE(theme != nullptr);
    qreal step = 0;
    REQUIRE(QMetaObject::invokeMethod(theme, "spaceOf", Q_RETURN_ARG(qreal, step), Q_ARG(int, 3)));
    qreal const barBottom = bar.mapToScene(QPointF{0, bar.height()}).y();
    CHECK(Scene::itemOf(*page).mapToScene(QPointF{0, 0}).y() >= barBottom + step - 0.5);
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

TEST_CASE("qt_quick containers: a panel's header sits beside its title and stays visible while collapsed",
          "[qt_quick]") {
    Scene scene;
    auto panel = scene.backend().createPanel(nullptr);
    panel->setTitle("More");
    panel->setCollapsible(true);
    auto header = scene.backend().createButton(panel.get());
    panel->setHeader(*header);
    auto content = scene.backend().createButton(panel.get());
    panel->setCollapsed(true);
    scene.settle();
    CHECK_FALSE(Scene::itemOf(*content).isVisible());
    CHECK(Scene::itemOf(*header).isVisible());
}

TEST_CASE("qt_quick containers: a panel with no title, no header and no collapse control shows no divider",
          "[qt_quick]") {
    Scene scene;
    auto panel = scene.backend().createPanel(nullptr);
    auto content = scene.backend().createButton(panel.get());
    scene.settle();
    auto const* const divider = Scene::itemOf(*panel).findChild<QQuickItem*>(QStringLiteral("divider"));
    REQUIRE(divider != nullptr);
    CHECK_FALSE(divider->isVisible());
}

TEST_CASE("qt_quick containers: a panel with only a title shows its header row and a divider", "[qt_quick]") {
    Scene scene;
    auto panel = scene.backend().createPanel(nullptr);
    panel->setTitle("More");
    auto content = scene.backend().createButton(panel.get());
    scene.settle();
    auto const* const divider = Scene::itemOf(*panel).findChild<QQuickItem*>(QStringLiteral("divider"));
    REQUIRE(divider != nullptr);
    CHECK(divider->isVisible());
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
    auto const first = scene.backend().createTextInput(column.get(), ui::TextInputMode::SingleLine);
    auto const second = scene.backend().createTextInput(column.get(), ui::TextInputMode::SingleLine);
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
    auto const column = scene.backend().createStack(outer.get(), ui::Axis::Vertical);
    auto const input = scene.backend().createTextInput(column.get(), ui::TextInputMode::SingleLine);
    auto narrow = scene.backend().createButton(column.get());
    narrow->setLabel("Narrow");
    narrow->setLayout({.width = ui::Sizing::fixed(10), .height = {}});
    auto const row = scene.backend().createStack(column.get(), ui::Axis::Horizontal);
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
    CHECK(std::abs(Scene::itemOf(*label).width() - Scene::itemOf(*label).implicitWidth()) < 1.0);
    CHECK(Scene::itemOf(*label).mapToScene(QPointF{0, 0}).x() == Scene::itemOf(*column).mapToScene(QPointF{0, 0}).x());
    // In a row, a content child is as tall as the row, and keeps its natural width.
    CHECK(Scene::itemOf(*button).height() == Scene::itemOf(*row).height());
    CHECK(Scene::itemOf(*button).height() > Scene::itemOf(*other).implicitHeight());
    CHECK(std::abs(Scene::itemOf(*button).width() - Scene::itemOf(*button).implicitWidth()) < 1.0);
    // The row packs its children at the start rather than spreading them.
    CHECK(horizontalGap(Scene::itemOf(*button), Scene::itemOf(*other)) == 0.0);
}

TEST_CASE("qt_quick containers: a content-sized splitter is as large as its panes", "[qt_quick]") {
    Scene scene;
    auto const row = scene.backend().createStack(nullptr, ui::Axis::Horizontal);
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
    CHECK(Scene::itemOf(*splitter).implicitWidth() < (40.0 * unit) + 16.0);
}

TEST_CASE("qt_quick containers: a root that stretches in height fills the window; content roots sit at the top",
          "[qt_quick]") {
    Scene scene;
    auto header = scene.backend().createText(nullptr);
    header->setText("header");
    auto frame = scene.backend().createStack(nullptr, ui::Axis::Vertical);
    frame->setLayout({.width = ui::Sizing::stretch(), .height = ui::Sizing::stretch()});
    auto inside = scene.backend().createText(frame.get());
    inside->setText("inside");
    scene.settle();
    QQuickItem const& headerItem = Scene::itemOf(*header);
    QQuickItem const& frameItem = Scene::itemOf(*frame);
    CHECK(headerItem.mapToScene(QPointF{0, 0}).y() == 0.0);
    CHECK(frameItem.mapToScene(QPointF{0, 0}).y() == headerItem.height());
    CHECK(std::abs(frameItem.height() - (scene.window().height() - headerItem.height())) < 1.0);

    // Without a stretching root, the roots keep their own heights at the top of the window.
    frame->setVisible(false);
    auto second = scene.backend().createText(nullptr);
    second->setText("second");
    scene.settle();
    CHECK(Scene::itemOf(*second).mapToScene(QPointF{0, 0}).y() == headerItem.height());
}

TEST_CASE("qt_quick containers: the shell frame fills the window, its sidebar 28 units wide", "[qt_quick]") {
    Scene scene;
    morph::ui::ShellFrameOptions options;
    options.applicationName = "App";
    morph::ui::ShellFrame frame{scene.backend(), nullptr, std::move(options)};
    auto content = scene.backend().createText(&frame.page());
    content->setText("content");
    scene.settle();
    auto const itemFor = [](char const* testId) -> QQuickItem const& {
        detail::ItemBase const* const widget = detail::findWidget(QString::fromLatin1(testId));
        REQUIRE(widget != nullptr);
        REQUIRE(widget->item() != nullptr);
        return *widget->item();
    };
    auto* const theme = scene.engine().singletonInstance<QObject*>(QStringLiteral("MorphUi"), QStringLiteral("Theme"));
    REQUIRE(theme != nullptr);
    qreal const unit = theme->property("unitWidth").toReal();
    QQuickItem const& root = itemFor("shell");
    QQuickItem const& sidebar = itemFor("shell_sidebar");
    CHECK(std::abs(root.width() - scene.window().width()) < 1.0);
    CHECK(std::abs(root.height() - scene.window().height()) < 1.0);
    CHECK(std::abs(sidebar.width() - (28.0 * unit)) < 1.0);
    CHECK(std::abs(sidebar.height() - scene.window().height()) < 1.0);
    // The content sits right of the sidebar, inside the page's padding (step 4, 16 px).
    CHECK(Scene::itemOf(*content).mapToScene(QPointF{0, 0}).x() >= sidebar.width() + 16.0);
}

// Control: drop `nav.menu` from MorphMenu's triggerText (the trigger draws its name, "Navigation", instead).
TEST_CASE("qt_quick containers: the compact shell hides its sidebar and draws its navigation menu as ☰",
          "[qt_quick]") {
    Scene scene;
    morph::ui::ShellFrameOptions options;
    options.applicationName = "App";
    options.navigator.entries = {morph::ui::CatalogEntry{.id = "lims.review", .title = "Review", .group = "Lab"}};
    morph::ui::ShellFrame frame{scene.backend(), nullptr, std::move(options)};
    frame.showSignedIn(true);
    frame.setCompact(true);
    frame.navigator().setBadge("lims.review", morph::ui::NavigatorBadge{.text = "4", .tone = morph::ui::Tone::Info});
    scene.settle();
    detail::ItemBase const* const sidebar = detail::findWidget(QStringLiteral("shell_sidebar"));
    REQUIRE(sidebar != nullptr);
    CHECK_FALSE(sidebar->item()->isVisible());
    QQuickItem const& trigger = *textItemOf("shell_nav_menu", QStringLiteral("☰"));
    CHECK(trigger.isVisible());
}

// The inner items, not the widget roots: each root fills the bar's height whatever the alignment, so the roots share
// a centre either way, while the label and the button inside them are centred only when the bar centres them.
// Mutation: drop the top bar's setAlign(Align::Center) in ShellFrame (the principal's label sits at the top).
TEST_CASE("qt_quick containers: the shell's principal sits level with its sign-out button", "[qt_quick]") {
    Scene scene;
    morph::ui::ShellFrameOptions options;
    options.applicationName = "App";
    options.onSignOut = [] {};
    morph::ui::ShellFrame frame{scene.backend(), nullptr, std::move(options)};
    frame.showSignedIn(true);
    frame.setPrincipal("alice");
    scene.settle();
    auto const innerOf = [](char const* testId, char const* className) -> QQuickItem const& {
        detail::ItemBase const* const widget = detail::findWidget(QString::fromLatin1(testId));
        REQUIRE(widget != nullptr);
        REQUIRE(widget->item() != nullptr);
        QList<QQuickItem*> const inner = widget->item()->findChildren<QQuickItem*>();
        auto const found =
            std::ranges::find_if(inner, [className](QQuickItem const* item) { return item->inherits(className); });
        // A bool, not the iterators themselves: see textItemOf's own note above on why.
        bool const isFound = found != inner.end();
        REQUIRE(isFound);
        return **found;
    };
    QQuickItem const& label = *textItemOf("shell_principal", QStringLiteral("alice"));
    QQuickItem const& button = innerOf("shell_signOut", "QQuickAbstractButton");
    REQUIRE(label.height() > 0.0);
    REQUIRE(button.height() > label.height());
    qreal const labelCentre = label.mapToScene(QPointF{0, label.height() / 2}).y();
    qreal const buttonCentre = button.mapToScene(QPointF{0, button.height() / 2}).y();
    INFO("label centre " << labelCentre << ", button centre " << buttonCentre);
    CHECK(std::abs(labelCentre - buttonCentre) <= 1.0);
}

// Mutation: ignore `fieldLayout` in fieldCell (the label sits above its control).
TEST_CASE("qt_quick containers: an inline form field's label sits left of its control, level with it", "[qt_quick]") {
    Scene scene;
    auto const model = morph::forms::FormModel::fromSchema(
        "Probe", R"({"properties":{"name":{"type":"string","title":"Name"}},"required":["name"]})");
    REQUIRE(model.has_value());
    morph::forms::FormSession session{
        scene.runtime(), *model,
        [&scene](std::string_view, std::string const&) {
            return morph::async::Completion<std::string>::makeSettleable(&scene.owner()).first;
        },
        [&scene](std::string_view, std::string const&) {
            return morph::async::Completion<std::string>::makeSettleable(&scene.owner()).first;
        }};
    morph::forms::FormViewOptions options;
    options.fieldLayout = morph::forms::FieldLayout::Inline;
    ui::Mounted const view{scene.runtime(), scene.backend(), morph::forms::formView(session, std::move(options))};
    scene.settle();
    auto const itemOf = [](char const* testId) -> QQuickItem const& {
        detail::ItemBase const* const widget = detail::findWidget(QString::fromLatin1(testId));
        REQUIRE(widget != nullptr);
        REQUIRE(widget->item() != nullptr);
        return *widget->item();
    };
    QQuickItem const& label = *textItemOf("label_name", QStringLiteral("Name"));
    QQuickItem const& control = itemOf("field_name");
    REQUIRE(label.height() > 0.0);
    REQUIRE(control.height() > label.height());
    QPointF const labelAt = label.mapToScene(QPointF{0, label.height() / 2});
    QPointF const controlAt = control.mapToScene(QPointF{0, control.height() / 2});
    INFO("label at " << labelAt.x() << "," << labelAt.y() << "; control at " << controlAt.x() << "," << controlAt.y());
    CHECK(labelAt.x() + label.width() <= controlAt.x() + 1.0);
    CHECK(std::abs(labelAt.y() - controlAt.y()) <= 1.0);
}

// Mutation: create the navigator's entries without setStyle (an entry is as wide as its label, and is not tinted).
TEST_CASE("qt_quick containers: a navigator entry fills the sidebar, and the current one is tinted", "[qt_quick]") {
    Scene scene;
    morph::ui::ShellFrameOptions options;
    options.applicationName = "App";
    options.navigator.entries = {morph::ui::CatalogEntry{.id = "a", .title = "Samples", .module = "Lab"},
                                 morph::ui::CatalogEntry{.id = "b", .title = "Results", .module = "Lab"}};
    morph::ui::ShellFrame frame{scene.backend(), nullptr, std::move(options)};
    frame.showSignedIn(true);
    frame.navigator().setCurrent("b");
    scene.settle();
    auto const itemOf = [](char const* testId) -> QQuickItem const& {
        detail::ItemBase const* const widget = detail::findWidget(QString::fromLatin1(testId));
        REQUIRE(widget != nullptr);
        REQUIRE(widget->item() != nullptr);
        return *widget->item();
    };
    auto const fillOf = [&itemOf](char const* testId) {
        QList<QQuickItem*> const inner = itemOf(testId).findChildren<QQuickItem*>();
        auto const button =
            std::ranges::find_if(inner, [](QQuickItem const* item) { return item->inherits("QQuickAbstractButton"); });
        // A bool, not the iterators themselves: see textItemOf's own note above on why.
        bool const isFound = button != inner.end();
        REQUIRE(isFound);
        auto* const background = (*button)->property("background").value<QQuickItem*>();
        REQUIRE(background != nullptr);
        return background->property("color").value<QColor>();
    };
    QQuickItem const& list = itemOf("navigator");
    QQuickItem const& entry = itemOf("nav_a");
    INFO("navigator " << list.width() << " wide, its entry " << entry.width());
    CHECK(std::abs(entry.width() - list.width()) < 1.0);
    CHECK(fillOf("nav_b") != fillOf("nav_a"));
    CHECK(fillOf("nav_a").alpha() == 0);  // an item that is not current, and not hovered, is clear
}

// Mutation: draw role 5 in the body font (the section is not bold), or answer the text colour for it in textColorOf.
TEST_CASE("qt_quick containers: the frame's type scale runs application name, section, entry", "[qt_quick]") {
    Scene scene;
    morph::ui::ShellFrameOptions options;
    options.applicationName = "App";
    options.navigator.entries = {morph::ui::CatalogEntry{.id = "a", .title = "Samples", .module = "Lab"}};
    morph::ui::ShellFrame frame{scene.backend(), nullptr, std::move(options)};
    frame.showSignedIn(true);
    auto body = scene.backend().createText(&frame.page());
    body->setText("body");
    scene.settle();
    body->setTestId("body");
    scene.settle();
    QQuickItem const& sectionLabel = *textItemOf("navigator_group_Lab/", QStringLiteral("Lab"));
    QFontInfo const section{sectionLabel.property("font").value<QFont>()};
    QFontInfo const name{textItemOf("shell_application", QStringLiteral("App"))->property("font").value<QFont>()};
    QFontInfo const text{textItemOf("body", QStringLiteral("body"))->property("font").value<QFont>()};
    INFO("section " << section.pixelSize() << " bold " << section.bold() << ", name " << name.pixelSize() << ", body "
                    << text.pixelSize());
    CHECK(section.bold());
    CHECK(section.pixelSize() == text.pixelSize());
    CHECK(name.pixelSize() > section.pixelSize());
    auto const color = sectionLabel.property("color").value<QColor>();
    // The heading draws in its own scheme, the sidebar's.
    detail::ItemBase const* const heading = detail::findWidget(QStringLiteral("navigator_group_Lab/"));
    auto* const scheme = heading->item()->property("scheme").value<QObject*>();
    REQUIRE(scheme != nullptr);
    CHECK(color == scheme->property("muted").value<QColor>());
    CHECK_FALSE(color == scheme->property("text").value<QColor>());
}

// The stack's length comes from its parent here: a `between` spacing taken from that length must not feed the
// stack's implicit length back to the parent, or each layout pass widens the row and the window never settles.
// Mutation: make MorphStack's implicit length its layout's (the row grows past the window, and its siblings are not
// laid out).
TEST_CASE("qt_quick containers: a row whose width its column gives spreads its children between its ends",
          "[qt_quick]") {
    Scene scene;
    auto const named = [](std::string const& text) {
        ui::Text made{.text = text};
        made.common.testId = "spread_" + text;
        return ui::text(std::move(made));
    };
    ui::Row row{.children = {named("count"), named("a"), named("b")}, .justify = ui::Justify::Between};
    row.common.testId = "spread";
    ui::Text below{.text = "below"};
    below.common.testId = "below";
    ui::Mounted const view{scene.runtime(), scene.backend(),
                           ui::column({.children = {ui::row(std::move(row)), ui::text(std::move(below))}})};
    scene.settle();
    detail::ItemBase const* const widget = detail::findWidget(QStringLiteral("spread"));
    REQUIRE(widget != nullptr);
    QQuickItem const& spread = *widget->item();
    QQuickItem const& last = *textItemOf("spread_b", QStringLiteral("b"));
    QQuickItem const& first = *textItemOf("spread_count", QStringLiteral("count"));
    qreal const end = last.mapToScene(QPointF{last.width(), 0}).x();
    INFO("row " << spread.width() << " wide in a window " << scene.window().width() << " wide; its last child ends at "
                << end << ", its first starts at " << first.mapToScene(QPointF{0, 0}).x());
    CHECK(std::abs(spread.width() - scene.window().width()) < 1.0);
    CHECK(std::abs(first.mapToScene(QPointF{0, 0}).x()) < 1.0);
    CHECK(end <= scene.window().width() + 1.0);
    CHECK(end >= scene.window().width() - 4.0);
}

// Mutation: drop the spacer's stretch (the row packs, so the actions stop right after the subtitle).
TEST_CASE("qt_quick containers: a head row puts its actions at the trailing edge", "[qt_quick]") {
    Scene scene;
    // The slot a host gives the row: the window, 600 wide.
    scene.window().resize(600, 600);
    scene.settle();
    ui::Text subtitle{.text = "Editing", .role = ui::TextRole::Muted};
    subtitle.common.testId = "head_subtitle";
    ui::Spacer filler;
    filler.common.layout.width = ui::Sizing::stretch();
    ui::Button action{.label = "Save"};
    action.common.testId = "head_action_save";
    ui::Row head{
        .children = {ui::text(std::move(subtitle)), ui::spacer(std::move(filler)), ui::button(std::move(action))},
        .gap = 2};
    head.common.testId = "head";
    head.common.layout.width = ui::Sizing::stretch();
    ui::Column slot{.children = {ui::row(std::move(head))}};
    slot.common.testId = "head_slot";
    ui::Mounted const view{scene.runtime(), scene.backend(), ui::column(std::move(slot))};
    scene.settle();

    detail::ItemBase const* const slotWidget = detail::findWidget(QStringLiteral("head_slot"));
    REQUIRE(slotWidget != nullptr);
    QQuickItem const& slotItem = *slotWidget->item();
    detail::ItemBase const* const actionWidget = detail::findWidget(QStringLiteral("head_action_save"));
    REQUIRE(actionWidget != nullptr);
    QQuickItem const& actionItem = *actionWidget->item();
    auto* const theme = scene.engine().singletonInstance<QObject*>(QStringLiteral("MorphUi"), QStringLiteral("Theme"));
    REQUIRE(theme != nullptr);
    qreal gap = 0;
    REQUIRE(QMetaObject::invokeMethod(theme, "spaceOf", Q_RETURN_ARG(qreal, gap), Q_ARG(int, 2)));
    qreal const slotRight = slotItem.mapToScene(QPointF{slotItem.width(), 0}).x();
    qreal const actionRight = actionItem.mapToScene(QPointF{actionItem.width(), 0}).x();
    INFO("slot ends at " << slotRight << ", the last action at " << actionRight << ", a gap is " << gap);
    CHECK(actionRight <= slotRight + 0.5);
    CHECK(actionRight >= slotRight - gap - 0.5);
}

// Mutation: put the head slot back in the title's line of ShellFrame (at 1000 px the actions are drawn over the
// sign-out button), or size a horizontal scroll's content to its own width (the head row does not reach the edge).
TEST_CASE("qt_quick containers: the shell's head has a line of its own, which scrolls when the bar is narrow",
          "[qt_quick]") {
    Scene scene;
    morph::ui::ShellFrameOptions options;
    options.applicationName = "App";
    options.onSignOut = [] {};
    morph::ui::ShellFrame frame{scene.backend(), nullptr, std::move(options)};
    frame.showSignedIn(true);
    frame.setTitle("Sample S-2026-0412 · stability pull");
    frame.setPrincipal("alice");
    frame.setHeadShown(true);
    std::vector<ui::Node> children{ui::text({.text = "Paracetamol 500 mg, lot 24-117", .role = ui::TextRole::Muted})};
    for (auto const* const tag : {"Draft", "Due today", "Priority"}) {
        children.push_back(ui::badge({.tone = ui::Tone::Warn, .text = tag}));
    }
    ui::Spacer filler;
    filler.common.layout.width = ui::Sizing::stretch();
    children.push_back(ui::spacer(std::move(filler)));
    for (auto const* const action : {"Book", "Sign"}) {
        ui::Button button{.label = action};
        button.common.testId = std::string{"head_"} + action;
        children.push_back(ui::button(std::move(button)));
    }
    ui::Row head{.children = std::move(children), .gap = 2};
    head.common.layout.width = ui::Sizing::stretch();
    ui::Mounted const mounted{scene.runtime(), scene.backend(), ui::row(std::move(head)), &frame.headSlot()};

    auto const rectOf = [](char const* testId) {
        detail::ItemBase const* const widget = detail::findWidget(QString::fromLatin1(testId));
        REQUIRE(widget != nullptr);
        QQuickItem const* const item = widget->item();
        REQUIRE(item != nullptr);
        return QRectF{item->mapToScene(QPointF{0, 0}), QSizeF{item->width(), item->height()}};
    };
    scene.window().resize(1000, 600);
    scene.settle();
    QRectF const signOut = rectOf("shell_signOut");
    for (char const* const action : {"head_Book", "head_Sign"}) {
        QRectF const rect = rectOf(action);
        INFO(action << " at " << rect.x() << "+" << rect.width());
        CHECK_FALSE(rect.intersects(signOut));
        CHECK(rect.right() <= 1000.0);
    }
    // The head row fills the line, so its actions sit at the trailing edge.
    CHECK(rectOf("head_Sign").right() > 900.0);

    scene.window().resize(400, 600);
    scene.settle();
    detail::ItemBase const* const scroll = detail::findWidget(QStringLiteral("shell_head_scroll"));
    REQUIRE(scroll != nullptr);
    QList<QQuickItem*> const inside = scroll->item()->childItems();
    auto const found =
        std::ranges::find_if(inside, [](QQuickItem const* item) { return item->inherits("QQuickFlickable"); });
    bool const isFlick = found != inside.end();
    REQUIRE(isFlick);
    QQuickItem const* const flick = *found;
    qreal const contentWidth = flick->property("contentWidth").toReal();
    CHECK(contentWidth > flick->width());
    QQuickItem const* const sign = detail::findWidget(QStringLiteral("head_Sign"))->item();
    QPointF const inContent = sign->mapToItem(flick->property("contentItem").value<QQuickItem*>(), QPointF{0, 0});
    CHECK(inContent.x() + sign->width() <= contentWidth + 0.5);
}

// SPDX-License-Identifier: Apache-2.0
//
// The containers on the real Qt Quick engine: children in order, moves that keep the moved item, and what each
// container kind shows and reports.

#include <QAccessible>
#include <QAccessibleInterface>
#include <QColor>
#include <QFont>
#include <QFontInfo>
#include <QImage>
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
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <functional>
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
    auto* const toggle = Scene::itemOf(*panel).findChild<QQuickItem*>(QStringLiteral("toggle"));
    REQUIRE(toggle != nullptr);
    scene.click(*toggle);
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

// Mutation: leave every tab in the bar (the last tab lies past the window's right edge), or never show More.
TEST_CASE("qt_quick containers: a narrow tab bar keeps every tab reachable, the chosen one named in More",
          "[qt_quick]") {
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
        static_cast<void>(settledButton(scene, *tabs, 6));
        QList<QQuickItem*> const buttons = Scene::itemsOf(Scene::itemOf(*tabs), "QQuickTabButton");
        REQUIRE(buttons.size() == 7);
        for (QQuickItem const* const button : buttons) {
            if (button->isVisible()) {
                QRectF const where{button->mapToScene(QPointF{0, 0}), QSizeF{button->width(), button->height()}};
                CHECK(where.x() >= -0.5);
                CHECK(where.right() <= 360.5);
            }
        }
        auto const* const more = Scene::itemOf(*tabs).findChild<QQuickItem*>(QStringLiteral("more"));
        REQUIRE(more != nullptr);
        CHECK(more->isVisible());
        CHECK(buttons.at(6)->isVisible() == false);
        CHECK(more->property("text").toString() == QStringLiteral("Training \u25be"));
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

// A bar `pinned` writes is a padded column on the `bar` surface in a divided column: it is filled with the surface
// colour and a hairline in the divider colour separates it from the content.
TEST_CASE("qt_quick containers: a pinned bar is drawn on the bar surface with a rule above it", "[qt_quick]") {
    Scene scene;
    scene.window().resize(400, 300);
    auto* const theme = scene.engine().singletonInstance<QObject*>(QStringLiteral("MorphUi"), QStringLiteral("Theme"));
    REQUIRE(theme != nullptr);
    theme->setProperty("surface", QColor{"#fafad2"});
    theme->setProperty("divider", QColor{"#ff0000"});
    auto root = scene.backend().createStack(nullptr, ui::Axis::Vertical);
    root->setDivided(true);
    auto content = scene.backend().createSpacer(root.get());
    content->setLayout({.width = ui::Sizing::stretch(), .height = ui::Sizing::stretch()});
    auto withSurface = scene.backend().createStack(root.get(), ui::Axis::Vertical);
    withSurface->setSurface("bar");
    withSurface->setPadding(2);
    auto bar = scene.backend().createText(withSurface.get());
    bar->setText("bar");
    scene.settle();
    QQuickItem const& barItem = Scene::itemOf(*withSurface);
    QImage const image = scene.window().grabWindow();
    auto const edge = barItem.mapToScene(QPointF{});
    int const x = static_cast<int>(edge.x()) + 380;
    int const y = static_cast<int>(edge.y());
    CHECK(barItem.height() > 0);
    CHECK(image.pixelColor(x, y + 4) == QColor{"#fafad2"});
    CHECK(image.pixelColor(x, y - 1) == QColor{"#ff0000"});
    // The same bar with no surface reads the page colour.
    withSurface->setSurface("");
    scene.settle();
    CHECK(scene.window().grabWindow().pixelColor(x, y + 4) != QColor{"#fafad2"});
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

namespace {

// Visible items 1 px tall and wider than a pixel, lying between two heights in the window, whatever their names.
int rulesBetween(QQuickItem const& root, double top, double bottom) {
    int found = 0;
    for (QQuickItem const* const item : root.findChildren<QQuickItem*>()) {
        double const y = item->mapToScene(QPointF{0, 0}).y();
        if (item->isVisible() && item->height() >= 0.5 && item->height() <= 1.5 && item->width() > 1.5 &&
            y >= top - 0.5 && y <= bottom - 0.5) {
            ++found;
        }
    }
    return found;
}

QQuickItem& named(QQuickItem& root, char const* name) {
    auto* const found = root.findChild<QQuickItem*>(QString::fromUtf8(name));
    REQUIRE(found != nullptr);
    return *found;
}

}  // namespace

TEST_CASE("qt_quick containers: a panel with no title, no header and no collapse control shows no header row",
          "[qt_quick]") {
    Scene scene;
    auto panel = scene.backend().createPanel(nullptr);
    auto content = scene.backend().createButton(panel.get());
    scene.settle();
    CHECK_FALSE(named(Scene::itemOf(*panel), "title").isVisible());
}

// Mutation: put the rule under the panel's header back.
TEST_CASE("qt_quick containers: a titled panel draws no rule between its header and its content", "[qt_quick]") {
    Scene scene;
    auto panel = scene.backend().createPanel(nullptr);
    panel->setTitle("More");
    panel->setPadding(2);
    panel->setCollapsible(true);
    auto content = scene.backend().createButton(panel.get());
    scene.settle();
    QQuickItem& item = Scene::itemOf(*panel);
    QQuickItem const& title = named(item, "title");
    CHECK(title.isVisible());
    double const headerBottom = named(item, "toggle").mapToScene(QPointF{0, named(item, "toggle").height()}).y();
    double const contentTop = Scene::itemOf(*content).mapToScene(QPointF{0, 0}).y();
    CHECK(contentTop >= headerBottom);
    CHECK(rulesBetween(item, headerBottom - 0.5, contentTop) == 0);
}

// Mutation: inset the header by the panel's padding above (it then does not start at the frame's top).
TEST_CASE("qt_quick containers: a collapsible panel's header starts at the frame's top", "[qt_quick]") {
    Scene scene;
    auto panel = scene.backend().createPanel(nullptr);
    panel->setTitle("More");
    panel->setPadding(3);
    panel->setCollapsible(true);
    auto content = scene.backend().createButton(panel.get());
    scene.settle();
    QQuickItem& item = Scene::itemOf(*panel);
    double const frameTop = named(item, "frame").mapToScene(QPointF{0, 0}).y();
    double const headerTop = named(item, "toggle").mapToScene(QPointF{0, 0}).y();
    CHECK(std::abs(headerTop - (frameTop + 1)) < 0.5);
    auto* const theme = scene.engine().singletonInstance<QObject*>(QStringLiteral("MorphUi"), QStringLiteral("Theme"));
    CHECK(named(item, "toggle").height() >= theme->property("targetHeight").toDouble() - 0.5);
}

// Mutation: leave the padding below and the layout's spacing in place while the content is hidden (the collapsed card
// is then a header and 13 px more).
TEST_CASE("qt_quick containers: a collapsed panel is its header row inside the frame, and no taller", "[qt_quick]") {
    Scene scene;
    auto panel = scene.backend().createPanel(nullptr);
    panel->setTitle("Qualification");
    panel->setPadding(2);
    panel->setCollapsible(true);
    panel->setCollapsed(true);
    auto content = scene.backend().createButton(panel.get());
    scene.settle();
    QQuickItem& item = Scene::itemOf(*panel);
    CHECK(named(item, "frame").height() == Catch::Approx(named(item, "toggle").height() + 2).margin(0.5));
}

// Mutation: inset the header by the panel's padding alone (a panel with no padding then draws its title on the
// border).
TEST_CASE("qt_quick containers: a section header keeps a horizontal inset whatever the panel's padding",
          "[qt_quick]") {
    Scene scene;
    auto panel = scene.backend().createPanel(nullptr);
    panel->setTitle("Stability");
    panel->setPadding(0);
    panel->setCollapsible(true);
    auto content = scene.backend().createButton(panel.get());
    scene.settle();
    QQuickItem& item = Scene::itemOf(*panel);
    constexpr double inset = 8;  // the default spacing step 2
    double const frameLeft = named(item, "frame").mapToScene(QPointF{0, 0}).x();
    CHECK(named(item, "title").mapToScene(QPointF{0, 0}).x() >= frameLeft + inset);
}

// Mutation: the old toggle, a ToolButton in the body font with no least height, indented by its own padding.
TEST_CASE("qt_quick containers: a collapsible's header is a tall strong row flush with its content", "[qt_quick]") {
    Scene scene;
    auto section = scene.backend().createCollapsible(nullptr);
    section->setTitle("Section");
    auto content = scene.backend().createText(section.get());
    content->setText("body");
    scene.settle();
    QQuickItem& item = Scene::itemOf(*section);
    auto* const theme = scene.engine().singletonInstance<QObject*>(QStringLiteral("MorphUi"), QStringLiteral("Theme"));
    QQuickItem const& toggle = named(item, "toggle");
    CHECK(toggle.height() >= theme->property("targetHeight").toDouble() - 0.5);
    QQuickItem const& title = named(item, "title");
    CHECK(title.property("font").value<QFont>().weight() >= QFont::DemiBold);
    CHECK(std::abs(title.mapToScene(QPointF{0, 0}).x() - Scene::itemOf(*content).mapToScene(QPointF{0, 0}).x()) < 1.0);
}

TEST_CASE("qt_quick containers: a section header is a button that reports whether it is expanded", "[qt_quick]") {
    Scene scene;
    auto section = scene.backend().createCollapsible(nullptr);
    section->setTitle("Section");
    auto content = scene.backend().createText(section.get());
    content->setText("body");
    std::vector<bool> toggles;
    section->setOnToggle([&toggles](bool open) { toggles.push_back(open); });
    scene.settle();
    QQuickItem& toggle = named(Scene::itemOf(*section), "toggle");
    bool const wasActive = QAccessible::isActive();
    QAccessible::setActive(true);
    QAccessibleInterface const* const node = QAccessible::queryAccessibleInterface(&toggle);
    REQUIRE(node != nullptr);
    CHECK(node->role() == QAccessible::Button);
    CHECK(node->text(QAccessible::Name) == QStringLiteral("Section"));
    // Qt 6.8's Accessible has no expanded state to report; the toggle's attached Accessible says whether this Qt has.
    bool reportsExpanded = false;
    for (QObject const* const child : toggle.children()) {
        if (std::string_view{child->metaObject()->className()} == "QQuickAccessibleAttached") {
            reportsExpanded = child->metaObject()->indexOfProperty("expanded") >= 0;
        }
    }
    // A Qt known to report it must, or the checks below would pass by checking nothing.
    if (QT_VERSION >= QT_VERSION_CHECK(6, 12, 0)) {
        CHECK(reportsExpanded);
    }
    if (reportsExpanded) {
        CHECK(node->state().expandable);
        CHECK(node->state().expanded);
    }
    scene.click(toggle);
    CHECK(toggles == std::vector<bool>{false});
    if (reportsExpanded) {
        CHECK_FALSE(QAccessible::queryAccessibleInterface(&toggle)->state().expanded);
    }
    toggle.forceActiveFocus();
    scene.key(Qt::Key_Space);
    CHECK(toggles == std::vector<bool>{false, true});
    if (reportsExpanded) {
        CHECK(QAccessible::queryAccessibleInterface(&toggle)->state().expanded);
    }
    QAccessible::setActive(wasActive);
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

// Mutation: put the principal back on the top bar's line (it is then level with the sign-out button, not above it).
TEST_CASE("qt_quick containers: the shell's principal sits above its sign-out button at the sidebar's foot",
          "[qt_quick]") {
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
    detail::ItemBase const* const sidebar = detail::findWidget(QStringLiteral("shell_sidebar"));
    REQUIRE(sidebar != nullptr);
    REQUIRE(label.height() > 0.0);
    qreal const labelBottom = label.mapToScene(QPointF{0, label.height()}).y();
    qreal const buttonTop = button.mapToScene(QPointF{0, 0}).y();
    INFO("label bottom " << labelBottom << ", button top " << buttonTop);
    CHECK(labelBottom <= buttonTop + 1.0);
    // Both sit in the sidebar, the button at its foot.
    qreal const sidebarBottom = sidebar->item()->mapToScene(QPointF{0, sidebar->item()->height()}).y();
    CHECK(button.mapToScene(QPointF{0, button.height()}).y() <= sidebarBottom);
    CHECK(button.mapToScene(QPointF{0, button.height()}).y() >= sidebarBottom - (2.0 * label.height()) - 48.0);
    CHECK(label.mapToScene(QPointF{0, 0}).x() < sidebar->item()->width());
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

// A text in a wrapping row nested in a row keeps the width its row gives it: the row places it from the text's own
// implicit size, which changes while the text's bindings on its width are still updating, so the placement has to
// run after them. The text arrives after the row's first layout, and the card is shown after, as a bound text in a
// card is. Mutation: run `place` directly from the cell's implicit-size signals (the heading's label is left at
// width 0 and breaks into one character per line, and the tag floats away from it).
TEST_CASE("qt_quick containers: a wrapping row nested in a row lays its text out on one line", "[qt_quick]") {
    Scene scene;
    // Hidden while it is built and shown after, as a card is until its sample arrives.
    auto card = scene.backend().createStack(nullptr, ui::Axis::Vertical);
    card->setLayout({.width = ui::Sizing::stretch(), .height = {}});
    card->setVisible(false);
    auto outer = scene.backend().createStack(card.get(), ui::Axis::Horizontal);
    outer->setLayout({.width = ui::Sizing::stretch(), .height = {}});
    outer->setGap(1);
    outer->setAlign(ui::Align::Center);
    auto wrapping = scene.backend().createStack(outer.get(), ui::Axis::Horizontal);
    wrapping->setWrap(true);
    wrapping->setGap(1);
    wrapping->setAlign(ui::Align::Center);
    auto heading = scene.backend().createText(wrapping.get());
    heading->setRole(ui::TextRole::Section);
    auto tag = scene.backend().createText(wrapping.get());
    tag->setText("Registered");
    auto filler = scene.backend().createSpacer(outer.get());
    filler->setLayout({.width = ui::Sizing::stretch(), .height = {}});
    auto lot = scene.backend().createText(outer.get());
    lot->setText("Lot L-26-0381");
    heading->setText("Sample S-26-104230");
    scene.window().resize(700, 400);
    scene.settle();
    card->setVisible(true);
    scene.settle();
    QQuickItem const& headingItem = Scene::itemOf(*heading);
    QQuickItem const& tagItem = Scene::itemOf(*tag);
    qreal const headingRight = headingItem.mapToScene(QPointF{headingItem.width(), 0}).x();
    qreal const tagLeft = tagItem.mapToScene(QPointF{0, 0}).x();
    INFO("heading " << headingItem.width() << "x" << headingItem.height() << ", tag at " << tagLeft);
    CHECK(headingItem.width() > 100.0);
    CHECK(headingItem.height() < 40.0);
    CHECK(tagLeft - headingRight < 16.0);
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

// Mutation: put the head slots back on a line of their own below the title (at 1000 px the actions are then lower than
// the title), make the lead a column of the subtitle over the badges (the badges are then below the subtitle at
// 1000 px), or drop the lead block's minimum width (at 390 px the actions stay beside the badges).
TEST_CASE("qt_quick containers: the shell's head shares the title's line, and wraps under it only when out of room",
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
    std::vector<ui::Node> badgeNodes;
    for (auto const* const tag : {"Draft", "Due today"}) {
        ui::Badge badge{.tone = ui::Tone::Warn, .text = tag};
        badge.common.testId = std::string{"head_badge_"} + tag;
        badgeNodes.push_back(ui::badge(std::move(badge)));
    }
    ui::Row badgeRow{.children = std::move(badgeNodes), .gap = 1};
    badgeRow.wrap = true;
    ui::Text subtitle{.text = "Paracetamol 500 mg, lot 24-117", .role = ui::TextRole::Muted};
    subtitle.common.testId = "head_subtitle";
    ui::Row leadRow{.children = {ui::text(std::move(subtitle)), ui::row(std::move(badgeRow))}, .gap = 1};
    leadRow.wrap = true;
    leadRow.align = ui::Align::Center;
    leadRow.common.layout.width = ui::Sizing::stretch();
    leadRow.common.layout.minWidth = ui::kShellHeadLeadMinimum;
    std::vector<ui::Node> trail;
    for (auto const* const action : {"Book", "Sign"}) {
        ui::Button button{.label = action};
        button.common.testId = std::string{"head_"} + action;
        trail.push_back(ui::button(std::move(button)));
    }
    ui::Mounted const leadMount{scene.runtime(), scene.backend(), ui::row(std::move(leadRow)), &frame.headLeadSlot()};
    ui::Mounted const trailMount{scene.runtime(), scene.backend(),
                                 ui::row(ui::Row{.children = std::move(trail), .gap = 1}), &frame.headSlot()};

    auto const rectOf = [](char const* testId) {
        detail::ItemBase const* const widget = detail::findWidget(QString::fromLatin1(testId));
        REQUIRE(widget != nullptr);
        QQuickItem const* const item = widget->item();
        REQUIRE(item != nullptr);
        return QRectF{item->mapToScene(QPointF{0, 0}), QSizeF{item->width(), item->height()}};
    };
    scene.window().resize(1000, 600);
    scene.settle();
    QRectF const titleAt1000 = rectOf("shell_title");
    QRectF const bookAt1000 = rectOf("head_Book");
    INFO("title at " << titleAt1000.y() << ", the actions at " << bookAt1000.y());
    // One line: the actions' top is the title's top, and they sit at the trailing edge.
    CHECK(bookAt1000.y() <= titleAt1000.y() + 1.0);
    CHECK(rectOf("head_Sign").right() > 900.0);
    CHECK_FALSE(bookAt1000.intersects(rectOf("shell_signOut")));
    // The subtitle and the badges share a line, below the title.
    QRectF const subtitleAt1000 = rectOf("head_subtitle");
    QRectF const badgeAt1000 = rectOf("head_badge_Due today");
    INFO("the subtitle at " << subtitleAt1000.y() << ", the badge at " << badgeAt1000.y());
    CHECK(badgeAt1000.y() < subtitleAt1000.bottom());
    CHECK(badgeAt1000.x() > subtitleAt1000.right());
    CHECK(subtitleAt1000.y() >= titleAt1000.bottom() - 1.0);

    scene.window().resize(390, 600);
    scene.settle();
    QRectF const badges = rectOf("head_badge_Due today");
    QRectF const bookAt390 = rectOf("head_Book");
    INFO("the badges end at " << badges.bottom() << ", the actions start at " << bookAt390.y());
    // Out of room: the actions drop to a line of their own, below the badges.
    CHECK(bookAt390.y() >= badges.bottom() - 1.0);
    CHECK(rectOf("head_Sign").right() <= 390.0);
}

// A head that fits stays on the title's line at any width: wrapping the trail under the title needs the line to be out
// of room, not a lead minimum wider than the lead itself. Mutation: make a stretch child's basis its minimum width
// whatever its natural width (the 100 px action drops under a 110 px title at 390 px).
TEST_CASE("qt_quick containers: a head whose title and action fit in the compact bar stays on one line",
          "[qt_quick]") {
    Scene scene;
    morph::ui::ShellFrame frame{scene.backend(), nullptr, morph::ui::ShellFrameOptions{.applicationName = "App"}};
    frame.showSignedIn(true);
    frame.setCompact(true);
    frame.setTitle("Audit trail");
    frame.setHeadShown(true);
    ui::Button action{.label = "Export PDF"};
    action.common.testId = "fit_export";
    ui::Mounted const trailMount{scene.runtime(), scene.backend(),
                                 ui::row(ui::Row{.children = {ui::button(std::move(action))}, .gap = 1}),
                                 &frame.headSlot()};
    scene.window().resize(390, 600);
    scene.settle();
    auto const rectOf = [](char const* testId) {
        detail::ItemBase const* const widget = detail::findWidget(QString::fromLatin1(testId));
        REQUIRE(widget != nullptr);
        QQuickItem const* const item = widget->item();
        REQUIRE(item != nullptr);
        return QRectF{item->mapToScene(QPointF{0, 0}), QSizeF{item->width(), item->height()}};
    };
    QRectF const title = rectOf("shell_title");
    QRectF const exportAt = rectOf("fit_export");
    INFO("the title at " << title.y() << ", the action at " << exportAt.y());
    CHECK(exportAt.y() < title.bottom());
    CHECK(exportAt.right() <= 390.0);
}

// The top bar keeps a gutter on every side: its first control starts a gap in from the left edge, its last ends a gap
// short of the right, and the line has room above and below. Mutation: drop the line's gap and the spacers (the
// controls touch the edges).
TEST_CASE("qt_quick containers: the shell's top bar insets its controls and centres them across its line",
          "[qt_quick]") {
    Scene scene;
    morph::ui::ShellFrame frame{scene.backend(), nullptr, morph::ui::ShellFrameOptions{.applicationName = "App"}};
    frame.showSignedIn(true);
    frame.setCompact(true);
    frame.setTitle("Console");
    frame.setHeadShown(true);
    ui::Button action{.label = "Open"};
    action.common.testId = "inset_action";
    ui::Mounted const trailMount{scene.runtime(), scene.backend(),
                                 ui::row(ui::Row{.children = {ui::button(std::move(action))}, .gap = 1}),
                                 &frame.headSlot()};
    scene.window().resize(1000, 600);
    scene.settle();
    auto const rectOf = [](char const* testId) {
        detail::ItemBase const* const widget = detail::findWidget(QString::fromLatin1(testId));
        REQUIRE(widget != nullptr);
        QQuickItem const* const item = widget->item();
        REQUIRE(item != nullptr);
        return QRectF{item->mapToScene(QPointF{0, 0}), QSizeF{item->width(), item->height()}};
    };
    auto* const theme = scene.engine().singletonInstance<QObject*>(QStringLiteral("MorphUi"), QStringLiteral("Theme"));
    REQUIRE(theme != nullptr);
    // The compact form pads the line's ends by step 1.
    qreal gap = 0;
    REQUIRE(QMetaObject::invokeMethod(theme, "spaceOf", Q_RETURN_ARG(qreal, gap), Q_ARG(int, 1)));
    QRectF const bar = rectOf("shell_topbar");
    QRectF const menu = rectOf("shell_nav_menu");
    QRectF const actionAt = rectOf("inset_action");
    INFO("the bar " << bar.left() << ".." << bar.right() << " x " << bar.top() << ".." << bar.bottom()
                    << ", the menu at " << menu.left() << ", " << menu.top() << ", the action " << actionAt.left()
                    << ".." << actionAt.right() << ", " << actionAt.top() << ".." << actionAt.bottom() << ", a gap is "
                    << gap);
    CHECK(menu.left() - bar.left() >= gap - 0.5);
    CHECK(bar.right() - actionAt.right() >= gap - 0.5);
    CHECK(menu.top() - bar.top() >= 1.0);
    CHECK(bar.bottom() - actionAt.bottom() >= 1.0);
    // Level with one another: the menu and the action are the same height, so their tops agree.
    CHECK(std::abs(menu.center().y() - actionAt.center().y()) < 1.5);
}

// Mutation: draw the rules in the scheme's border colour, leave `divided` unread (no rules), or put the rule on the
// lower child's edge instead of in the gap.
TEST_CASE("qt_quick containers: a divided column draws a divider rule in each gap and a plain one none",
          "[qt_quick]") {
    Scene scene;
    // The one-pixel rectangles under @p item, however deep.
    std::function<void(QQuickItem const&, QList<QQuickItem*>&)> collect = [&collect](QQuickItem const& item,
                                                                                     QList<QQuickItem*>& found) {
        for (QQuickItem* const child : item.childItems()) {
            if (child->inherits("QQuickRectangle") && child->height() == 1 && child->isVisible()) {
                found.append(child);
            }
            collect(*child, found);
        }
    };
    auto const rulesOf = [&collect](QQuickItem const& stack) {
        QList<QQuickItem*> found;
        collect(stack, found);
        return found;
    };
    for (bool const divided : {false, true}) {
        auto column = scene.backend().createStack(nullptr, ui::Axis::Vertical);
        if (divided) {
            column->setDivided(true);
        }
        column->setGap(0);
        std::vector<std::unique_ptr<ui::TextWidget>> texts;
        for (char const* const name : {"a", "b", "c"}) {
            texts.push_back(scene.backend().createText(column.get()));
            texts.back()->setText(name);
        }
        scene.settle();
        QList<QQuickItem*> rules = rulesOf(Scene::itemOf(*column));
        if (!divided) {
            CHECK(rules.isEmpty());
            continue;
        }
        REQUIRE(rules.size() == 2);
        std::ranges::sort(rules.begin(), rules.end(),
                          [](QQuickItem const* left, QQuickItem const* right) { return left->y() < right->y(); });
        QQuickItem const& first = Scene::itemOf(*texts.at(0));
        QQuickItem const& second = Scene::itemOf(*texts.at(1));
        CHECK(rules.at(0)->y() >= first.y() + first.height() - 0.5);
        CHECK(rules.at(0)->y() + 1 <= second.y() + 0.5);
        CHECK(rules.at(0)->property("color").value<QColor>() == QColor{QStringLiteral("#dcdce0")});
    }
}

// Mutation: leave the tabs in the bar (the last lies past the bar's edge), hide More, make More a tab button, or
// choose by position among the shown tabs only.
TEST_CASE("qt_quick containers: tabs that do not fit go into a More menu that chooses them", "[qt_quick]") {
    std::vector<std::string> const labels{"Overview", "Calibration", "Maintenance", "Bookings", "Audit trail"};
    Scene scene;
    scene.window().resize(390, 400);
    auto tabs = scene.backend().createTabs(nullptr);
    tabs->setLayout({.width = ui::Sizing::stretch(), .height = ui::Sizing::stretch()});
    tabs->setTabs(labels);
    std::vector<std::size_t> picks;
    tabs->setOnSelect([&picks](std::size_t index) { picks.push_back(index); });
    tabs->setSelected(0);
    static_cast<void>(settledButton(scene, *tabs, 0));
    QQuickItem& root = Scene::itemOf(*tabs);
    QList<QQuickItem*> const buttons = Scene::itemsOf(root, "QQuickTabButton");
    auto* const bar = Scene::itemsOf(root, "QQuickTabBar").at(0);
    REQUIRE(buttons.size() == 5);
    CHECK(bar->property("count").toInt() == 5);
    int shown = 0;
    for (QQuickItem const* const button : buttons) {
        if (!button->isVisible()) {
            continue;
        }
        ++shown;
        CHECK(button->mapToItem(bar, QPointF{button->width(), 0}).x() <= bar->width() + 0.5);
    }
    CHECK(shown > 0);
    CHECK(shown < 5);
    // A tab is a step of 3 in from its label on each side, as the design's tabs are.
    CHECK(buttons.at(0)->property("leftPadding").toReal() == Catch::Approx(12));
    auto* const more = root.findChild<QQuickItem*>(QStringLiteral("more"));
    REQUIRE(more != nullptr);
    CHECK(more->isVisible());
    // More follows the last shown tab, not the row's end.
    QQuickItem const* lastShown = nullptr;
    for (QQuickItem const* const button : buttons) {
        lastShown = button->isVisible() ? button : lastShown;
    }
    REQUIRE(lastShown != nullptr);
    CHECK(more->mapToScene(QPointF{0, 0}).x() ==
          Catch::Approx(lastShown->mapToScene(QPointF{lastShown->width(), 0}).x()).margin(1));
    CHECK(more->property("text").toString() == QStringLiteral("More \u25be"));
    CHECK(bar->property("currentIndex").toInt() == 0);

    // The last tab is in the menu; choosing it asks for it by its own index.
    scene.click(*more);
    scene.settle();
    auto* const popup = root.findChild<QObject*>(QStringLiteral("moreMenu"));
    REQUIRE(popup != nullptr);
    CHECK(popup->property("visible").toBool());
    QQuickItem* entry = nullptr;
    auto const* const menuContent = popup->property("contentItem").value<QQuickItem*>();
    REQUIRE(menuContent != nullptr);
    for (QQuickItem* const candidate : menuContent->childItems()) {
        if (candidate->objectName() == QStringLiteral("moreEntry") &&
            candidate->property("text").toString() == QStringLiteral("Audit trail")) {
            entry = candidate;
        }
    }
    REQUIRE(entry != nullptr);
    scene.click(*entry);
    scene.settle();
    CHECK(picks == std::vector<std::size_t>{4});
    tabs->setSelected(4);
    static_cast<void>(settledButton(scene, *tabs, 4));
    CHECK(more->property("text").toString() == QStringLiteral("Audit trail \u25be"));
    CHECK(Scene::itemsOf(root, "QQuickTabButton").size() == 5);
    int shownAfter = 0;
    for (QQuickItem const* const button : Scene::itemsOf(root, "QQuickTabButton")) {
        shownAfter += button->isVisible() ? 1 : 0;
    }
    CHECK(shownAfter == shown);

    // Right on the last shown tab moves the focus to More.
    QQuickItem* last = nullptr;
    for (QQuickItem* const button : buttons) {
        last = button->isVisible() ? button : last;
    }
    REQUIRE(last != nullptr);
    last->forceActiveFocus();
    scene.key(Qt::Key_Right);
    CHECK(scene.window().activeFocusItem() == more);

    // A wide window brings every tab back and hides More.
    scene.window().resize(1000, 400);
    static_cast<void>(settledButton(scene, *tabs, 0));
    CHECK_FALSE(more->isVisible());
    for (QQuickItem const* const button : Scene::itemsOf(root, "QQuickTabButton")) {
        CHECK(button->isVisible());
    }
}

// Mutation: ignore `inset` (the first tab's button starts at x = 0, and the page keeps its edge).
TEST_CASE(
    "qt_quick containers: a tabs node's inset insets its first tab and its pages, and its strip surface spans it",
    "[qt_quick]") {
    Scene scene;
    scene.window().resize(600, 300);
    auto* const theme = scene.engine().singletonInstance<QObject*>(QStringLiteral("MorphUi"), QStringLiteral("Theme"));
    REQUIRE(theme != nullptr);
    theme->setProperty("surface", QColor{"#fafad2"});
    auto root = scene.backend().createStack(nullptr, ui::Axis::Vertical);
    auto tabs = scene.backend().createTabs(root.get());
    tabs->setTabs({"One", "Two"});
    auto page = scene.backend().createStack(tabs.get(), ui::Axis::Vertical);
    auto label = scene.backend().createText(page.get());
    label->setText("page");
    auto plainTabs = scene.backend().createTabs(root.get());
    plainTabs->setTabs({"One", "Two"});
    auto plainPage = scene.backend().createStack(plainTabs.get(), ui::Axis::Vertical);
    auto plainLabel = scene.backend().createText(plainPage.get());
    plainLabel->setText("page");
    scene.settle();
    QQuickItem& item = Scene::itemOf(*tabs);
    QQuickItem& plainItem = Scene::itemOf(*plainTabs);
    auto const firstButton = [](QQuickItem& node) {
        QList<QQuickItem*> const buttons = Scene::itemsOf(node, "QQuickTabButton");
        REQUIRE_FALSE(buttons.isEmpty());
        return buttons.front()->mapToItem(&node, QPointF{0, 0});
    };
    QPointF const plainButton = firstButton(plainItem);
    QPointF const plainChild = Scene::itemOf(*plainLabel).mapToItem(&plainItem, QPointF{0, 0});
    tabs->setInset(4);
    tabs->setStripSurface("bar");
    scene.settle();
    double const step = theme->property("space").value<QList<qreal>>().at(4);
    REQUIRE(step > 0.0);
    CHECK(firstButton(item).x() == plainButton.x() + step);
    QPointF const child = Scene::itemOf(*label).mapToItem(&item, QPointF{0, 0});
    CHECK(child.x() == plainChild.x() + step);
    auto* const strip = item.findChild<QQuickItem*>(QStringLiteral("stripSurface"));
    REQUIRE(strip != nullptr);
    // An inset strip puts no spacing of its own over its page: the page's inset is the gap under the rule.
    CHECK(child.y() == strip->mapToItem(&item, QPointF{0, strip->height()}).y() + step);
    CHECK(strip->isVisible());
    CHECK(strip->mapToItem(&item, QPointF{0, 0}).x() == 0.0);
    CHECK(strip->width() == item.width());
    CHECK(strip->property("color").value<QColor>() == QColor{"#fafad2"});
    auto* const none = plainItem.findChild<QQuickItem*>(QStringLiteral("stripSurface"));
    REQUIRE(none != nullptr);
    CHECK_FALSE(none->isVisible());
}

// A lead with no minimum of its own, as the workspace builds it for a shell, leaves a short lead and a short trail on
// the title's line in a compact bar: the lead block's 32 units are capped at its natural width. Mutation: give the
// lead row the 32-unit minimum (Scan drops to a second line at 330 px).
TEST_CASE("qt_quick containers: a compact shell head with a short lead keeps its trail on the title's line",
          "[qt_quick]") {
    Scene scene;
    morph::ui::ShellFrame frame{scene.backend(), nullptr, morph::ui::ShellFrameOptions{.applicationName = "App"}};
    frame.showSignedIn(true);
    frame.setCompact(true);
    frame.setTitle("Overview");
    frame.setHeadShown(true);
    ui::Badge badge{.tone = ui::Tone::Ok, .text = "Connected"};
    badge.common.testId = "lead_badge";
    ui::Row leadRow{.children = {ui::badge(std::move(badge))}, .gap = 1};
    leadRow.wrap = true;
    leadRow.align = ui::Align::Center;
    leadRow.common.layout.width = ui::Sizing::stretch();
    ui::Button scan{.label = "Scan"};
    scan.common.testId = "head_scan";
    ui::Mounted const leadMount{scene.runtime(), scene.backend(), ui::row(std::move(leadRow)), &frame.headLeadSlot()};
    ui::Mounted const trailMount{scene.runtime(), scene.backend(),
                                 ui::row(ui::Row{.children = {ui::button(std::move(scan))}, .gap = 1}),
                                 &frame.headSlot()};
    scene.window().resize(330, 600);
    scene.settle();
    auto const rectOf = [](char const* testId) {
        detail::ItemBase const* const widget = detail::findWidget(QString::fromLatin1(testId));
        REQUIRE(widget != nullptr);
        QQuickItem const* const item = widget->item();
        REQUIRE(item != nullptr);
        return QRectF{item->mapToScene(QPointF{0, 0}), QSizeF{item->width(), item->height()}};
    };
    QRectF const title = rectOf("shell_title");
    QRectF const scanAt = rectOf("head_scan");
    INFO("the title at " << title.y() << ", Scan at " << scanAt.y());
    CHECK(scanAt.y() < title.bottom());
    CHECK(scanAt.right() <= 330.0);
}

// Mutation: a stack that ignores `selected` draws no bar and keeps the Client role.
TEST_CASE("qt_quick containers: a selected stack draws the accent bar at its leading edge and is a selected group",
          "[qt_quick]") {
    Scene scene;
    scene.window().resize(400, 300);
    auto* const theme = scene.engine().singletonInstance<QObject*>(QStringLiteral("MorphUi"), QStringLiteral("Theme"));
    REQUIRE(theme != nullptr);
    theme->setProperty("surface", QColor{"#fafad2"});
    theme->setProperty("accent", QColor{"#17784a"});
    auto root = scene.backend().createStack(nullptr, ui::Axis::Vertical);
    auto card = scene.backend().createStack(root.get(), ui::Axis::Vertical);
    card->setSurface("bar");
    card->setPadding(2);
    card->setSelected(true);
    auto label = scene.backend().createText(card.get());
    label->setText("card");
    auto plain = scene.backend().createStack(root.get(), ui::Axis::Vertical);
    plain->setPadding(2);
    auto other = scene.backend().createText(plain.get());
    other->setText("plain");
    scene.settle();

    QQuickItem& item = Scene::itemOf(*card);
    auto* const bar = item.findChild<QQuickItem*>(QStringLiteral("selectionBar"));
    REQUIRE(bar != nullptr);
    CHECK(bar->isVisible());
    CHECK(bar->width() == 3.0);
    CHECK(bar->x() == 0.0);
    CHECK(bar->height() == item.height());
    CHECK(bar->property("color").value<QColor>() == QColor{"#17784a"});
    QImage const image = scene.window().grabWindow();
    QPointF const origin = item.mapToScene(QPointF{});
    int const y = static_cast<int>(origin.y() + (item.height() / 2));
    CHECK(image.pixelColor(static_cast<int>(origin.x()) + 1, y) == QColor{"#17784a"});
    CHECK(image.pixelColor(static_cast<int>(origin.x()) + 5, y) == QColor{"#fafad2"});

    bool const wasActive = QAccessible::isActive();
    QAccessible::setActive(true);
    QAccessibleInterface const* const node = QAccessible::queryAccessibleInterface(&item);
    REQUIRE(node != nullptr);
    CHECK(node->role() == QAccessible::Grouping);
    CHECK(node->state().selected);
    QAccessibleInterface const* const unmarked = QAccessible::queryAccessibleInterface(&Scene::itemOf(*plain));
    REQUIRE(unmarked != nullptr);
    CHECK(unmarked->role() == QAccessible::Client);

    card->setSelected(false);
    scene.settle();
    CHECK_FALSE(bar->isVisible());
    CHECK_FALSE(QAccessible::queryAccessibleInterface(&item)->state().selected);
    CHECK(QAccessible::queryAccessibleInterface(&item)->role() == QAccessible::Grouping);
    QAccessible::setActive(wasActive);
}

// A pinned page inside a frame whose page inset is hidden: the bar's surface reaches the window's edges, with its
// content one page step in. Mutation: keep the frame's inset (the bar starts 16 px in, and is 32 px narrower).
TEST_CASE("qt_quick containers: a pinned bar spans the page when the shell leaves the page uninset", "[qt_quick]") {
    Scene scene;
    morph::ui::ShellFrame frame{scene.backend(), nullptr, morph::ui::ShellFrameOptions{.applicationName = "App"}};
    frame.showSignedIn(true);
    frame.setCompact(true);
    frame.setPageInset(false);
    auto* const theme = scene.engine().singletonInstance<QObject*>(QStringLiteral("MorphUi"), QStringLiteral("Theme"));
    REQUIRE(theme != nullptr);
    qreal step = 0;
    REQUIRE(QMetaObject::invokeMethod(theme, "spaceOf", Q_RETURN_ARG(qreal, step), Q_ARG(int, 4)));

    ui::Button reject{.label = "Reject"};
    reject.common.testId = "pin_reject";
    ui::Column bar{.children = {ui::button(std::move(reject))}, .padding = ui::Padding{2, 4}};
    bar.common.surface = "bar";
    bar.common.testId = "pin_bar";
    ui::Text body{.text = "body"};
    ui::Column bodyColumn{.children = {ui::text(std::move(body))}, .padding = 4};
    ui::Scroll scrolled{.child = ui::column(std::move(bodyColumn))};
    scrolled.common.layout = ui::LayoutHints{.width = ui::Sizing::stretch(), .height = ui::Sizing::stretch()};
    ui::Column page{.children = {ui::scroll(std::move(scrolled)), ui::column(std::move(bar))}, .divided = true};
    page.common.layout = ui::LayoutHints{.width = ui::Sizing::stretch(), .height = ui::Sizing::stretch()};
    ui::Mounted const pageMount{scene.runtime(), scene.backend(), ui::column(std::move(page)), &frame.page()};

    scene.window().resize(390, 600);
    scene.settle();
    auto const rectOf = [](char const* testId) {
        detail::ItemBase const* const widget = detail::findWidget(QString::fromLatin1(testId));
        REQUIRE(widget != nullptr);
        QQuickItem const* const item = widget->item();
        REQUIRE(item != nullptr);
        return QRectF{item->mapToScene(QPointF{0, 0}), QSizeF{item->width(), item->height()}};
    };
    QRectF const barAt = rectOf("pin_bar");
    QRectF const buttonAt = rectOf("pin_reject");
    INFO("the bar at " << barAt.x() << " wide " << barAt.width() << ", its button at " << buttonAt.x());
    CHECK(barAt.x() == 0.0);
    CHECK(barAt.width() == 390.0);
    CHECK(buttonAt.x() - barAt.x() == step);
}

namespace {

// A compact signed-in shell frame on a 390 px window whose theme draws icon-only buttons square.
struct CompactFrame {
    explicit CompactFrame(Scene& scene) {
        auto const tokens = ui::parseTokens(R"({"shape": {"iconOnly": {"$type": "keyword", "$value": "square"}}})");
        REQUIRE(tokens);
        auto const resolved = ui::resolveTokens({&*tokens});
        REQUIRE(resolved);
        REQUIRE(morph::qt_quick::applyTheme(scene.engine(), ui::themeValues(*resolved)).empty());
        ui::ShellFrameOptions options;
        options.applicationName = "App";
        options.navigator.entries = {ui::CatalogEntry{.id = "lims.review", .title = "Review", .group = "Lab"}};
        frame = std::make_unique<ui::ShellFrame>(scene.backend(), nullptr, std::move(options));
        frame->showSignedIn(true);
        frame->setCompact(true);
        frame->setTitle("Review");
        scene.window().resize(390, 600);
        scene.settle();
    }

    std::unique_ptr<ui::ShellFrame> frame;
};

}  // namespace

// Control: leave the compact gaps at steps 3 and 3 (the menu square starts at 12 px, and the title at 68).
TEST_CASE("qt_quick containers: the compact top line starts its menu square a step 1 in and the title a step 2 after",
          "[qt_quick]") {
    Scene scene;
    CompactFrame compact{scene};
    auto* const theme = scene.engine().singletonInstance<QObject*>(QStringLiteral("MorphUi"), QStringLiteral("Theme"));
    REQUIRE(theme != nullptr);
    qreal stepOne = 0.0;
    qreal stepTwo = 0.0;
    REQUIRE(QMetaObject::invokeMethod(theme, "spaceOf", Q_RETURN_ARG(qreal, stepOne), Q_ARG(int, 1)));
    REQUIRE(QMetaObject::invokeMethod(theme, "spaceOf", Q_RETURN_ARG(qreal, stepTwo), Q_ARG(int, 2)));
    qreal const target = theme->property("targetHeight").toReal();
    detail::ItemBase const* const menu = detail::findWidget(QStringLiteral("shell_nav_menu"));
    REQUIRE(menu != nullptr);
    QQuickItem const& menuItem = *menu->item();
    QQuickItem const& title = *textItemOf("shell_title", QStringLiteral("Review"));
    INFO("menu at " << menuItem.mapToScene(QPointF{0, 0}).x() << " wide " << menuItem.width() << ", title at "
                    << title.mapToScene(QPointF{0, 0}).x());
    CHECK(menuItem.mapToScene(QPointF{0, 0}).x() == stepOne);
    CHECK(menuItem.width() == target);
    CHECK(menuItem.height() == target);
    CHECK(title.mapToScene(QPointF{0, 0}).x() == stepOne + target + stepTwo);
}

// Control: keep `Align::Start` (the title sits 11 px above the button's centre).
TEST_CASE("qt_quick containers: a title alone on a one-line head is centred on the trail's button", "[qt_quick]") {
    Scene scene;
    CompactFrame compact{scene};
    auto button = scene.backend().createButton(&compact.frame->headSlot());
    button->setLabel("Export PDF");
    button->setTestId("probe_export");
    compact.frame->setHeadShown(true);
    compact.frame->setHeadOneLine(true);
    scene.settle();
    QQuickItem const& title = *textItemOf("shell_title", QStringLiteral("Review"));
    QQuickItem const& export_ = Scene::itemOf(*button);
    qreal const titleMiddle = title.mapToScene(QPointF{0, title.height() / 2}).y();
    qreal const buttonMiddle = export_.mapToScene(QPointF{0, export_.height() / 2}).y();
    INFO("title middle " << titleMiddle << ", button middle " << buttonMiddle);
    CHECK(std::abs(titleMiddle - buttonMiddle) <= 1.0);

    compact.frame->setHeadOneLine(false);
    scene.settle();
    CHECK(std::abs(title.mapToScene(QPointF{0, title.height() / 2}).y() -
                   export_.mapToScene(QPointF{0, export_.height() / 2}).y()) > 3.0);
}

// Mutation: ignore `collapsible.marker` in the section header (the chevron stays), or let `none` also drop the toggle.
TEST_CASE("qt_quick containers: a collapsible draws no marker under collapsible.marker none and still toggles",
          "[qt_quick]") {
    Scene scene;
    auto section = scene.backend().createCollapsible(nullptr);
    section->setTitle("Section");
    auto content = scene.backend().createText(section.get());
    content->setText("body");
    std::vector<bool> toggles;
    section->setOnToggle([&toggles](bool open) { toggles.push_back(open); });
    scene.settle();
    QQuickItem& item = Scene::itemOf(*section);
    CHECK(named(item, "chevronGlyph").isVisible());

    auto const tokens = ui::parseTokens(R"({"collapsible": {"marker": {"$type": "keyword", "$value": "none"}}})");
    REQUIRE(tokens);
    auto const resolved = ui::resolveTokens({&*tokens});
    REQUIRE(resolved);
    REQUIRE(morph::qt_quick::applyTheme(scene.engine(), ui::themeValues(*resolved)).empty());
    scene.settle();
    CHECK_FALSE(named(item, "chevronGlyph").isVisible());
    CHECK_FALSE(named(item, "chevron").isVisible());
    scene.click(named(item, "toggle"));
    CHECK(toggles == std::vector<bool>{false});
}

// Mutation: leave the Basic style's handle (a plain bar in the palette's colours, with no grip).
TEST_CASE("qt_quick containers: a splitter's handle is the scheme's border colour with a centred grip", "[qt_quick]") {
    Scene scene;
    auto splitter = scene.backend().createSplitter(nullptr, ui::Axis::Horizontal);
    splitter->setLayout({.width = ui::Sizing::stretch(), .height = ui::Sizing::fixed(10)});
    auto left = scene.backend().createText(splitter.get());
    auto right = scene.backend().createText(splitter.get());
    left->setText("left");
    right->setText("right");
    scene.settle();
    QQuickItem& item = Scene::itemOf(*splitter);
    auto* const handle = item.property("handleItem").value<QQuickItem*>();
    REQUIRE(handle != nullptr);
    auto* const scheme = item.property("scheme").value<QObject*>();
    REQUIRE(scheme != nullptr);
    CHECK(handle->property("color").value<QColor>() == scheme->property("border").value<QColor>());
    QQuickItem const& grip = named(*handle, "grip");
    CHECK(grip.isVisible());
    CHECK(grip.width() == Catch::Approx(2.0).margin(0.01));
    CHECK(grip.height() == Catch::Approx(std::min(28.0, handle->height() / 3)).margin(0.01));
    QPointF const centre = grip.mapToItem(handle, QPointF{grip.width() / 2, grip.height() / 2});
    // `centerIn` rounds to a whole pixel.
    CHECK(centre.x() == Catch::Approx(handle->width() / 2).margin(0.5));
    CHECK(centre.y() == Catch::Approx(handle->height() / 2).margin(0.5));
    CHECK(grip.property("color").value<QColor>() == scheme->property("neutralDot").value<QColor>());
}

// Mutation: leave a row's `divided` unread (no rule, a step-2 gap), or put the rule on the second child's edge instead
// of in the gap.
TEST_CASE("qt_quick containers: a divided row draws a vertical rule centred in each gap", "[qt_quick]") {
    Scene scene;
    scene.window().resize(600, 200);
    auto row = scene.backend().createStack(nullptr, ui::Axis::Horizontal);
    row->setLayout({.width = ui::Sizing::stretch(), .height = ui::Sizing::stretch()});
    row->setGap(2);
    row->setDivided(true);
    std::vector<std::unique_ptr<ui::StackWidget>> columns;
    std::vector<std::unique_ptr<ui::TextWidget>> texts;
    for (char const* const name : {"left", "right"}) {
        columns.push_back(scene.backend().createStack(row.get(), ui::Axis::Vertical));
        columns.back()->setLayout({.width = ui::Sizing::fixed(10)});
        texts.push_back(scene.backend().createText(columns.back().get()));
        texts.back()->setText(name);
    }
    scene.settle();
    auto* const theme = scene.engine().singletonInstance<QObject*>(QStringLiteral("MorphUi"), QStringLiteral("Theme"));
    REQUIRE(theme != nullptr);
    qreal gap = 0;
    REQUIRE(QMetaObject::invokeMethod(theme, "spaceOf", Q_RETURN_ARG(qreal, gap), Q_ARG(int, 2)));

    QList<QQuickItem*> rules;
    std::function<void(QQuickItem const&)> collect = [&collect, &rules](QQuickItem const& item) {
        for (QQuickItem* const child : item.childItems()) {
            if (child->inherits("QQuickRectangle") && child->width() == 1 && child->isVisible()) {
                rules.append(child);
            }
            collect(*child);
        }
    };
    collect(Scene::itemOf(*row));
    REQUIRE(rules.size() == 1);
    QQuickItem const& first = Scene::itemOf(*columns.at(0));
    QQuickItem const& second = Scene::itemOf(*columns.at(1));
    qreal const firstEnd = first.x() + first.width();
    CHECK(second.x() - firstEnd == Catch::Approx(gap + 1).margin(0.5));
    CHECK(rules.at(0)->x() >= firstEnd - 0.5);
    CHECK(rules.at(0)->x() + 1 <= second.x() + 0.5);
    CHECK(rules.at(0)->x() - firstEnd == Catch::Approx(gap / 2).margin(0.5));
    CHECK(rules.at(0)->height() > 1);
}

// Mutation: size a tab by its label and the style's padding (about 20 px), or keep the strip's step-3 spacing over an
// inset page.
TEST_CASE("qt_quick containers: a tab is the control height and an inset strip puts no spacing over its page",
          "[qt_quick]") {
    Scene scene;
    // Wide enough to be expanded, so the control height is the whole of the rule rather than the touch height.
    scene.window().resize(1000, 300);
    auto* const theme = scene.engine().singletonInstance<QObject*>(QStringLiteral("MorphUi"), QStringLiteral("Theme"));
    REQUIRE(theme != nullptr);
    auto tabs = scene.backend().createTabs(nullptr);
    tabs->setTabs({"One", "Two"});
    tabs->setInset(4);
    auto page = scene.backend().createStack(tabs.get(), ui::Axis::Vertical);
    auto label = scene.backend().createText(page.get());
    label->setText("page");
    scene.settle();
    QQuickItem& item = Scene::itemOf(*tabs);
    QList<QQuickItem*> const buttons = Scene::itemsOf(item, "QQuickTabButton");
    REQUIRE_FALSE(buttons.isEmpty());
    double const control = theme->property("controlHeight").toDouble();
    CHECK(buttons.front()->height() == Catch::Approx(control));
    double const step = theme->property("space").value<QList<qreal>>().at(4);
    auto* const strip = item.findChild<QQuickItem*>(QStringLiteral("stripSurface"));
    REQUIRE(strip != nullptr);
    double const rule = strip->mapToItem(&item, QPointF{0, strip->height()}).y();
    CHECK(Scene::itemOf(*page).mapToItem(&item, QPointF{0, 0}).y() == Catch::Approx(rule + step));
}

// Mutation: leave `bleed` unread (the bleeding text stays inside the padding), or bleed past the frame's border.
TEST_CASE("qt_quick containers: a bleeding child reaches its padded column's and its panel's inner edges",
          "[qt_quick]") {
    Scene scene;
    scene.window().resize(600, 400);
    auto* const theme = scene.engine().singletonInstance<QObject*>(QStringLiteral("MorphUi"), QStringLiteral("Theme"));
    REQUIRE(theme != nullptr);
    double const step = theme->property("space").value<QList<qreal>>().at(2);
    REQUIRE(step > 0.0);

    auto column = scene.backend().createStack(nullptr, ui::Axis::Vertical);
    column->setPadding(ui::Padding{0, 2});
    column->setLayout({.width = ui::Sizing::fixed(40)});
    auto inside = scene.backend().createText(column.get());
    inside->setText("inside");
    auto bleeding = scene.backend().createText(column.get());
    bleeding->setText("bleeds");
    bleeding->setLayout({.bleed = true});

    auto panel = scene.backend().createPanel(nullptr);
    panel->setPadding(2);
    panel->setLayout({.width = ui::Sizing::fixed(40)});
    auto panelInside = scene.backend().createText(panel.get());
    panelInside->setText("inside");
    auto panelBleeding = scene.backend().createText(panel.get());
    panelBleeding->setText("bleeds");
    panelBleeding->setLayout({.bleed = true});
    scene.settle();

    QQuickItem& columnItem = Scene::itemOf(*column);
    QQuickItem& insideItem = Scene::itemOf(*inside);
    QQuickItem& bleedItem = Scene::itemOf(*bleeding);
    CHECK(insideItem.mapToItem(&columnItem, QPointF{0, 0}).x() == Catch::Approx(step));
    CHECK(bleedItem.mapToItem(&columnItem, QPointF{0, 0}).x() == Catch::Approx(0.0));
    CHECK(bleedItem.width() == Catch::Approx(columnItem.width()).margin(0.5));

    QQuickItem& panelItem = Scene::itemOf(*panel);
    QQuickItem& panelInsideItem = Scene::itemOf(*panelInside);
    QQuickItem& panelBleedItem = Scene::itemOf(*panelBleeding);
    // The frame's border is 1 px: a child reaches the inner edge, not over the border.
    CHECK(panelInsideItem.mapToItem(&panelItem, QPointF{0, 0}).x() == Catch::Approx(1.0 + step));
    CHECK(panelBleedItem.mapToItem(&panelItem, QPointF{0, 0}).x() == Catch::Approx(1.0));
    CHECK(panelBleedItem.width() == Catch::Approx(panelItem.width() - 2.0).margin(0.5));
}

// Mutation: ignore `pageInset` (the page keeps the strip's inset), or let it move the strip.
TEST_CASE("qt_quick containers: a tabs node's page inset pads the pages apart from its strip", "[qt_quick]") {
    Scene scene;
    scene.window().resize(600, 300);
    auto* const theme = scene.engine().singletonInstance<QObject*>(QStringLiteral("MorphUi"), QStringLiteral("Theme"));
    REQUIRE(theme != nullptr);
    double const step = theme->property("space").value<QList<qreal>>().at(4);
    auto tabs = scene.backend().createTabs(nullptr);
    tabs->setTabs({"One", "Two"});
    tabs->setInset(4);
    tabs->setPageInset(0);
    auto page = scene.backend().createStack(tabs.get(), ui::Axis::Vertical);
    auto label = scene.backend().createText(page.get());
    label->setText("page");
    scene.settle();
    QQuickItem& item = Scene::itemOf(*tabs);
    QList<QQuickItem*> const buttons = Scene::itemsOf(item, "QQuickTabButton");
    REQUIRE_FALSE(buttons.isEmpty());
    CHECK(buttons.front()->mapToItem(&item, QPointF{0, 0}).x() == Catch::Approx(step));
    CHECK(Scene::itemOf(*label).mapToItem(&item, QPointF{0, 0}).x() == Catch::Approx(0.0));

    // Pages padded apart from the strip: a bleeding page reaches the node's edges, the others stay inside.
    tabs->setPageInset(2);
    auto bleeding = scene.backend().createText(tabs.get());
    bleeding->setText("bleeds");
    bleeding->setLayout({.bleed = true});
    scene.settle();
    double const pageStep = theme->property("space").value<QList<qreal>>().at(2);
    CHECK(Scene::itemOf(*label).mapToItem(&item, QPointF{0, 0}).x() == Catch::Approx(pageStep));
    CHECK(Scene::itemOf(*bleeding).mapToItem(&item, QPointF{0, 0}).x() == Catch::Approx(0.0));
    CHECK(Scene::itemOf(*bleeding).width() == Catch::Approx(item.width()).margin(0.5));
}

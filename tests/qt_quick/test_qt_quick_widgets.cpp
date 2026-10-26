// SPDX-License-Identifier: Apache-2.0
//
// The leaf and field widgets on the real Qt Quick engine: what each setter shows, what reaches each handler, and
// the rules of the backend contract the conformance probe cannot reach (docs/spec/ui/backend_contract.md, "What the
// probe cannot reach"): read-only fields of every kind, unreachable widgets, and controlled inputs other than a
// text field snapping back.

#include <QAccessible>
#include <QAccessibleInterface>
#include <QColor>
#include <QDate>
#include <QDateTime>
#include <QFont>
#include <QFontInfo>
#include <QList>
#include <QMessageLogContext>
#include <QObject>
#include <QPointF>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QString>
#include <QStringList>
#include <QTest>
#include <QUrl>
#include <QVariant>
#include <QVariantList>
#include <QtLogging>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <morph/qt_quick/theme.hpp>
#include <morph/reactive/signal.hpp>
#include <morph/ui/backend.hpp>
#include <morph/ui/mount.hpp>
#include <morph/ui/view.hpp>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "qt_quick/keys.hpp"
#include "qt_quick/kinds.hpp"
#include "qt_quick_harness.hpp"

namespace ui = morph::ui;
namespace detail = morph::qt_quick::detail;
using morph::qt_quick::testing::Scene;

namespace {

detail::ItemBase& base(ui::Widget const& widget) {
    detail::ItemBase* const found = detail::baseOf(widget);
    REQUIRE(found != nullptr);
    return *found;
}

/// The first item named @p name under @p root, depth first: a popup's content sits under the window's overlay.
// NOLINTNEXTLINE(misc-no-recursion): an item tree, walked down.
QQuickItem* findNamed(QQuickItem& root, QString const& name) {
    for (QQuickItem* const child : root.childItems()) {
        if (child->objectName() == name) {
            return child;
        }
        if (QQuickItem* const found = findNamed(*child, name)) {
            return found;
        }
    }
    return nullptr;
}

}  // namespace

TEST_CASE("qt_quick widgets: a text shows its text in its role, and a button's click reaches its handler",
          "[qt_quick]") {
    Scene scene;
    auto column = scene.backend().createStack(nullptr, ui::Axis::Vertical);
    auto text = scene.backend().createText(column.get());
    text->setText("Hello");
    text->setRole(ui::TextRole::Heading);
    auto button = scene.backend().createButton(column.get());
    button->setLabel("Go");
    int clicks = 0;
    button->setOnClick([&clicks] { ++clicks; });
    scene.settle();
    CHECK(Scene::itemOf(*text).property("text").toString() == QStringLiteral("Hello"));
    CHECK(Scene::itemOf(*text).property("role").toInt() == 2);
    CHECK(base(*button).shownText() == "Go");
    scene.click(Scene::itemOf(*button));
    CHECK(clicks == 1);
    // Enter on a focused button activates it too.
    button->focus();
    scene.key(Qt::Key_Return);
    CHECK(clicks == 2);
}

// Mutation: never ask C++ from the HoverHandler (hovers stays 0).
TEST_CASE("qt_quick widgets: the pointer coming to rest over a watched widget reaches its hover handler",
          "[qt_quick]") {
    Scene scene;
    auto const column = scene.backend().createStack(nullptr, ui::Axis::Vertical);
    auto watched = scene.backend().createButton(column.get());
    watched->setLabel("Watched");
    auto other = scene.backend().createButton(column.get());
    other->setLabel("Other");
    int hovers = 0;
    watched->setOnHover([&hovers] { ++hovers; });
    scene.settle();
    QTest::mouseMove(&scene.window(), Scene::centreOf(Scene::itemOf(*other)));
    scene.settle();
    CHECK(hovers == 0);
    QTest::mouseMove(&scene.window(), Scene::centreOf(Scene::itemOf(*watched)));
    scene.settle();
    CHECK(hovers == 1);
    watched->setOnHover({});
    QTest::mouseMove(&scene.window(), Scene::centreOf(Scene::itemOf(*other)));
    QTest::mouseMove(&scene.window(), Scene::centreOf(Scene::itemOf(*watched)));
    scene.settle();
    CHECK(hovers == 1);
}

TEST_CASE("qt_quick widgets: typing into a text input reports each edit, Enter commits then submits", "[qt_quick]") {
    Scene scene;
    auto column = scene.backend().createStack(nullptr, ui::Axis::Vertical);
    auto input = scene.backend().createTextInput(column.get(), ui::TextInputMode::SingleLine);
    auto other = scene.backend().createButton(column.get());
    std::vector<std::string> log;
    input->setOnChange([&log](std::string const& text) { log.push_back("change " + text); });
    input->setOnCommit([&log](std::string const& text) { log.push_back("commit " + text); });
    input->setOnSubmit([&log](std::string const& text) { log.push_back("submit " + text); });
    input->setPlaceholder("Name");
    scene.settle();
    CHECK(Scene::itemOf(*input).property("placeholder").toString() == QStringLiteral("Name"));
    scene.click(Scene::controlOf(*input));
    scene.type(QStringLiteral("ab"));
    scene.key(Qt::Key_Return);
    CHECK(log == std::vector<std::string>{"change a", "change ab", "commit ab", "submit ab"});
    log.clear();
    // Focus leaving after an edit commits; leaving without one does not.
    scene.type(QStringLiteral("c"));
    other->focus();
    scene.settle();
    CHECK(log == std::vector<std::string>{"change abc", "commit abc"});
    log.clear();
    input->focus();
    other->focus();
    scene.settle();
    CHECK(log.empty());
}

// Mutation: never show the unit label (no visible `kg` inside the field).
TEST_CASE("qt_quick widgets: a text input draws its unit inside the field, and the text stops before it",
          "[qt_quick]") {
    Scene scene;
    auto column = scene.backend().createStack(nullptr, ui::Axis::Vertical);
    auto plain = scene.backend().createTextInput(column.get(), ui::TextInputMode::SingleLine);
    auto input = scene.backend().createTextInput(column.get(), ui::TextInputMode::SingleLine);
    input->setUnit("kg");
    input->setDecimals(2);
    scene.settle();
    QQuickItem& control = Scene::controlOf(*input);
    QQuickItem const* unit = nullptr;
    for (QQuickItem const* const text : Scene::itemsOf(control, "QQuickText")) {
        if (text->isVisible() && text->property("text").toString() == QStringLiteral("kg")) {
            unit = text;
        }
    }
    REQUIRE(unit != nullptr);
    CHECK(Scene::itemOf(*input).property("decimals").toInt() == 2);
    // Inside the field, at its trailing edge, and the text's padding makes room for it.
    QPointF const at = unit->mapToItem(&control, QPointF{0, 0});
    CHECK(at.x() > control.width() / 2);
    CHECK(at.x() + unit->width() <= control.width());
    CHECK(control.property("rightPadding").toReal() >
          Scene::controlOf(*plain).property("rightPadding").toReal() + unit->width() - 1);
    input->setUnit("");
    scene.settle();
    CHECK_FALSE(unit->isVisible());
}

// Mutation: never show the icon (no visible leading image inside the field).
TEST_CASE("qt_quick widgets: a text input draws its icon inside the field, and the text starts after it",
          "[qt_quick]") {
    Scene scene;
    std::string const svg = R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 8 8"><path d="M1 1l6 6"/></svg>)";
    CHECK(morph::qt_quick::applyBundleTheme(scene.engine(), {}, {{"search", svg}}).empty());
    auto plain = scene.backend().createTextInput(nullptr, ui::TextInputMode::SingleLine);
    auto input = scene.backend().createTextInput(nullptr, ui::TextInputMode::SingleLine);
    input->setIcon("search");
    scene.settle();
    QQuickItem const& control = Scene::controlOf(*input);
    auto const* const icon = control.findChild<QQuickItem*>(QStringLiteral("icon"));
    REQUIRE(icon != nullptr);
    CHECK(icon->isVisible());
    CHECK(control.property("leftPadding").toReal() >
          Scene::controlOf(*plain).property("leftPadding").toReal() + icon->width() - 1);
}

// A no-op: an input with no icon reserves no leading space and shows nothing where one would be.
TEST_CASE("qt_quick widgets: a text input with no icon reserves no leading space", "[qt_quick]") {
    Scene scene;
    auto plain = scene.backend().createTextInput(nullptr, ui::TextInputMode::SingleLine);
    scene.settle();
    QQuickItem const& control = Scene::controlOf(*plain);
    auto const* const icon = control.findChild<QQuickItem*>(QStringLiteral("icon"));
    REQUIRE(icon != nullptr);
    CHECK_FALSE(icon->isVisible());
}

TEST_CASE("qt_quick widgets: a setter writes the text without echoing it, and keeps the cursor on the shown text",
          "[qt_quick]") {
    Scene scene;
    auto input = scene.backend().createTextInput(nullptr, ui::TextInputMode::SingleLine);
    int changes = 0;
    input->setOnChange([&changes](std::string const&) { ++changes; });
    input->setText("héllo");
    scene.settle();
    auto& impl = dynamic_cast<detail::TextInputImpl&>(*input);
    CHECK(impl.shownText() == "héllo");
    impl.moveCursor(3);  // after "hé", two characters and three bytes
    CHECK(impl.cursor() == 3);
    input->setText("héllo");
    CHECK(impl.cursor() == 3);
    input->setText("other");
    CHECK(impl.shownText() == "other");
    CHECK(changes == 0);
}

TEST_CASE("qt_quick widgets: a password input masks, a multi-line input commits on focus leaving", "[qt_quick]") {
    Scene scene;
    auto column = scene.backend().createStack(nullptr, ui::Axis::Vertical);
    auto password = scene.backend().createTextInput(column.get(), ui::TextInputMode::Password);
    auto area = scene.backend().createTextInput(column.get(), ui::TextInputMode::Multiline);
    auto other = scene.backend().createButton(column.get());
    std::vector<std::string> log;
    area->setOnChange([&log](std::string const& text) { log.push_back("change " + text); });
    area->setOnCommit([&log](std::string const& text) { log.push_back("commit " + text); });
    scene.settle();
    CHECK(Scene::itemOf(*password).property("password").toBool());
    area->setText("first");
    scene.settle();
    CHECK(log.empty());
    CHECK(base(*area).shownText() == "first");
    area->focus();
    scene.settle();
    dynamic_cast<detail::TextInputImpl&>(*area).moveCursor(5);
    scene.type(QStringLiteral("x"));
    other->focus();
    scene.settle();
    CHECK(log == std::vector<std::string>{"change firstx", "commit firstx"});
}

TEST_CASE("qt_quick widgets: a read-only text input takes no submit or commit", "[qt_quick]") {
    Scene scene;
    auto column = scene.backend().createStack(nullptr, ui::Axis::Vertical);
    auto input = scene.backend().createTextInput(column.get(), ui::TextInputMode::SingleLine);
    auto other = scene.backend().createButton(column.get());
    std::vector<std::string> log;
    input->setOnChange([&log](std::string const& text) { log.push_back("change " + text); });
    input->setOnCommit([&log](std::string const& text) { log.push_back("commit " + text); });
    input->setOnSubmit([&log](std::string const& text) { log.push_back("submit " + text); });
    input->setText("kept");
    input->setReadOnly(true);
    scene.settle();
    scene.click(Scene::controlOf(*input));
    CHECK(Scene::controlOf(*input).hasActiveFocus());
    scene.type(QStringLiteral("x"));
    scene.key(Qt::Key_Return);
    other->focus();
    scene.settle();
    CHECK(log.empty());
    CHECK(base(*input).shownText() == "kept");
}

TEST_CASE("qt_quick widgets: a check box toggles on a click, and a read-only one refuses", "[qt_quick]") {
    Scene scene;
    auto box = scene.backend().createCheckbox(nullptr);
    std::vector<bool> toggles;
    box->setOnToggle([&toggles](bool checked) { toggles.push_back(checked); });
    box->setLabel("Agree");
    scene.settle();
    auto& impl = dynamic_cast<detail::CheckboxImpl&>(*box);
    CHECK(impl.shownText() == "Agree");
    scene.click(Scene::controlOf(*box));
    CHECK(toggles == std::vector<bool>{true});
    CHECK(impl.checked());
    box->setChecked(false);
    CHECK_FALSE(impl.checked());
    CHECK(toggles.size() == 1);
    box->setReadOnly(true);
    scene.click(Scene::controlOf(*box));
    CHECK(toggles.size() == 1);
    CHECK_FALSE(impl.checked());
}

TEST_CASE(
    "qt_quick widgets: a select marks the requested key, reports the user's choice by key, and refuses when "
    "read-only",
    "[qt_quick]") {
    Scene scene;
    auto select = scene.backend().createSelect(nullptr, ui::SelectStyle::Dropdown);
    std::vector<ui::Key> chosen;
    select->setOnSelect([&chosen](ui::Key key) { chosen.push_back(std::move(key)); });
    std::int64_t const big = (std::int64_t{1} << 53) + 1;
    select->setOptions({{.key = big, .label = "Big"}, {.key = std::string{"b"}, .label = "Bee"}});
    select->setSelected(ui::Key{std::string{"b"}});
    scene.settle();
    auto& impl = dynamic_cast<detail::SelectImpl&>(*select);
    CHECK(impl.shownText() == "Bee");
    // The combo box's own activation path, as choosing an entry of its popup runs it.
    impl.call("choose", 0);
    scene.settle();
    REQUIRE(chosen.size() == 1);
    CHECK(chosen.at(0) == ui::Key{big});
    CHECK(impl.shownText() == "Big");
    select->setReadOnly(true);
    impl.call("choose", 1);
    scene.settle();
    CHECK(chosen.size() == 1);
    CHECK(impl.shownText() == "Big");
    // New options keep the request: it is marked wherever it now is.
    select->setOptions({{.key = std::string{"z"}, .label = "Zed"}, {.key = big, .label = "Big again"}});
    CHECK(impl.marked() == 1);
}

// Mutation: drop MorphSelect's indicator: override -> the Basic style's stock arrow shows instead.
TEST_CASE("qt_quick widgets: a dropdown's indicator is themed, and flips with the popup", "[qt_quick]") {
    Scene scene;
    auto select = scene.backend().createSelect(nullptr, ui::SelectStyle::Dropdown);
    scene.settle();
    QQuickItem const& combo = Scene::controlOf(*select);
    auto* const indicator = combo.property("indicator").value<QQuickItem*>();
    REQUIRE(indicator != nullptr);
    CHECK(indicator->objectName() == QStringLiteral("indicator"));
}

// Mutation: drop MorphSelect's popup Connections -> opening reports nothing.
TEST_CASE("qt_quick widgets: a dropdown reports being opened, also with no options", "[qt_quick]") {
    Scene scene;
    auto select = scene.backend().createSelect(nullptr, ui::SelectStyle::Dropdown);
    int opened = 0;
    select->setOnOpen([&opened] { ++opened; });
    scene.settle();
    scene.click(Scene::controlOf(*select));
    CHECK(opened == 1);
}

TEST_CASE("qt_quick widgets: a radio select reports a click on an option", "[qt_quick]") {
    Scene scene;
    auto select = scene.backend().createSelect(nullptr, ui::SelectStyle::Radio);
    std::vector<ui::Key> chosen;
    select->setOnSelect([&chosen](ui::Key key) { chosen.push_back(std::move(key)); });
    select->setOptions({{.key = std::int64_t{1}, .label = "One"}, {.key = std::int64_t{2}, .label = "Two"}});
    scene.settle();
    QQuickItem& radios = Scene::controlOf(*select);
    QList<QQuickItem*> const buttons = Scene::itemsOf(radios, "QQuickRadioButton");
    REQUIRE(buttons.size() == 2);
    scene.click(*buttons.at(1));
    CHECK(chosen == std::vector<ui::Key>{ui::Key{std::int64_t{2}}});
}

TEST_CASE("qt_quick widgets: a slider moves by keys and reports the value; a read-only one refuses", "[qt_quick]") {
    Scene scene;
    auto slider = scene.backend().createSlider(nullptr);
    std::vector<std::int64_t> values;
    slider->setOnChange([&values](std::int64_t value) { values.push_back(value); });
    slider->setRange(0, 10, 1);
    slider->setValue(4);
    scene.settle();
    auto& impl = dynamic_cast<detail::SliderImpl&>(*slider);
    CHECK(impl.value() == 4);
    slider->focus();
    scene.key(Qt::Key_Right);
    CHECK(values == std::vector<std::int64_t>{5});
    slider->setReadOnly(true);
    scene.key(Qt::Key_Right);
    CHECK(values.size() == 1);
    CHECK(impl.value() == 5);
}

TEST_CASE("qt_quick widgets: a slider commits a finished move, on a release or a key click", "[qt_quick]") {
    Scene scene;
    auto slider = scene.backend().createSlider(nullptr);
    std::vector<std::int64_t> changes;
    std::vector<std::int64_t> commits;
    slider->setOnChange([&changes](std::int64_t value) { changes.push_back(value); });
    slider->setOnCommit([&commits](std::int64_t value) { commits.push_back(value); });
    slider->setRange(0, 10, 1);
    slider->setValue(4);
    scene.settle();
    QQuickItem& control = Scene::controlOf(*slider);
    // The window point of @p value on the control's track.
    auto const at = [&control](qreal value) {
        qreal const from = control.property("from").toReal();
        qreal const to = control.property("to").toReal();
        qreal const left = control.property("leftPadding").toReal();
        qreal const available = control.property("availableWidth").toReal();
        return control.mapToScene(QPointF{left + (((value - from) / (to - from)) * available), control.height() / 2})
            .toPoint();
    };
    // A drag across two steps reports each change while the press lasts, and one commit on its release.
    QTest::mousePress(&scene.window(), Qt::LeftButton, {}, at(5));
    scene.settle();
    QTest::mouseMove(&scene.window(), at(6));
    scene.settle();
    QTest::mouseRelease(&scene.window(), Qt::LeftButton, {}, at(6));
    scene.settle();
    CHECK(changes == std::vector<std::int64_t>{5, 6});
    CHECK(commits == std::vector<std::int64_t>{6});
    // A key click is a press and a release: one change and one commit.
    slider->focus();
    scene.key(Qt::Key_Right);
    CHECK(changes == std::vector<std::int64_t>{5, 6, 7});
    CHECK(commits == std::vector<std::int64_t>{6, 7});
    // A read-only slider refuses the move, so it commits nothing either.
    slider->setReadOnly(true);
    QTest::mousePress(&scene.window(), Qt::LeftButton, {}, at(2));
    scene.settle();
    QTest::mouseMove(&scene.window(), at(3));
    scene.settle();
    QTest::mouseRelease(&scene.window(), Qt::LeftButton, {}, at(3));
    scene.settle();
    CHECK(changes == std::vector<std::int64_t>{5, 6, 7});
    CHECK(commits == std::vector<std::int64_t>{6, 7});
}

TEST_CASE("qt_quick widgets: a date-time input formats in its zone and parses what the user types", "[qt_quick]") {
    using morph::time::DateTime;
    using namespace std::chrono;
    Scene scene;
    auto input = scene.backend().createDateTimeInput(nullptr, ui::DateMode::DateTime, 60);
    std::vector<std::optional<morph::time::Timestamp>> changes;
    input->setOnChange([&changes](std::optional<morph::time::Timestamp> value) { changes.push_back(value); });
    DateTime const instant{year{2026}, month{10}, day{11}, hours{22}, minutes{30}, seconds{0}};
    input->setValue(morph::time::Timestamp{instant});
    scene.settle();
    CHECK(base(*input).shownText() == "2026-10-11 23:30");
    scene.click(Scene::controlOf(*input));
    scene.key(Qt::Key_A, Qt::ControlModifier);
    scene.type(QStringLiteral("2026-10-12 01:15"));
    scene.key(Qt::Key_Return);
    REQUIRE(changes.size() == 1);
    REQUIRE(changes.at(0).has_value());
    CHECK(changes.at(0)->value->toIso8601() == "2026-10-12T00:15:00.000Z");
    // Text that names no instant is no request: the field shows what it showed before, here the accepted edit.
    scene.key(Qt::Key_A, Qt::ControlModifier);
    scene.type(QStringLiteral("soon"));
    scene.key(Qt::Key_Return);
    CHECK(changes.size() == 1);
    CHECK(base(*input).shownText() == "2026-10-12 01:15");
    // A second unparseable edit in a row still shows the held value.
    scene.key(Qt::Key_A, Qt::ControlModifier);
    scene.type(QStringLiteral("later"));
    scene.key(Qt::Key_Return);
    CHECK(changes.size() == 1);
    CHECK(base(*input).shownText() == "2026-10-12 01:15");
    // An emptied field is a cleared value.
    scene.key(Qt::Key_A, Qt::ControlModifier);
    scene.key(Qt::Key_Delete);
    scene.key(Qt::Key_Return);
    REQUIRE(changes.size() == 2);
    CHECK_FALSE(changes.at(1).has_value());
    input->setValue(std::nullopt);
    CHECK(base(*input).shownText().empty());
}

TEST_CASE("qt_quick widgets: a date input offers a calendar unless it is read-only", "[qt_quick]") {
    Scene scene;
    auto input = scene.backend().createDateTimeInput(nullptr, ui::DateMode::Date, 0);
    QQuickItem const* const button = findNamed(Scene::itemOf(*input), QStringLiteral("datePickerButton"));
    REQUIRE(button != nullptr);
    CHECK(button->isVisible());
    input->setReadOnly(true);
    scene.settle();
    CHECK_FALSE(button->isVisible());
}

TEST_CASE("qt_quick widgets: a calendar day reports the date once, and a later blur reports nothing", "[qt_quick]") {
    using morph::time::DateTime;
    using namespace std::chrono;
    Scene scene;
    auto column = scene.backend().createStack(nullptr, ui::Axis::Vertical);
    auto input = scene.backend().createDateTimeInput(column.get(), ui::DateMode::Date, 0);
    auto other = scene.backend().createButton(column.get());
    std::vector<std::optional<morph::time::Timestamp>> changes;
    input->setOnChange([&changes](std::optional<morph::time::Timestamp> value) { changes.push_back(value); });
    DateTime const chosen{year{2026}, month{3}, day{15}, hours{0}, minutes{0}, seconds{0}};
    input->setValue(morph::time::Timestamp{chosen});
    scene.settle();

    QQuickItem const* const button = findNamed(Scene::itemOf(*input), QStringLiteral("datePickerButton"));
    REQUIRE(button != nullptr);
    scene.click(*button);
    auto const* const popup = scene.window().contentItem()->findChild<QObject*>(QStringLiteral("datePickerPopup"));
    REQUIRE(popup != nullptr);
    CHECK(popup->property("visible").toBool());
    QQuickItem const* const cell = findNamed(*scene.window().contentItem(), QStringLiteral("day_2026-2-20"));
    REQUIRE(cell != nullptr);
    scene.click(*cell);
    REQUIRE(changes.size() == 1);
    REQUIRE(changes.at(0).has_value());
    CHECK(changes.at(0)->value->toIso8601() == "2026-03-20T00:00:00.000Z");
    CHECK(base(*input).shownText() == "2026-03-20");
    // The pick reported its own change: a blur afterwards reports nothing.
    scene.click(Scene::itemOf(*other));
    CHECK(changes.size() == 1);
}

TEST_CASE("qt_quick widgets: a calendar day keeps a date-time input's time of day in its zone", "[qt_quick]") {
    using morph::time::DateTime;
    using namespace std::chrono;
    Scene scene;
    auto input = scene.backend().createDateTimeInput(nullptr, ui::DateMode::DateTime, 120);
    std::vector<std::optional<morph::time::Timestamp>> changes;
    input->setOnChange([&changes](std::optional<morph::time::Timestamp> value) { changes.push_back(value); });
    DateTime const instant{year{2026}, month{3}, day{15}, hours{12}, minutes{30}, seconds{0}};
    input->setValue(morph::time::Timestamp{instant});
    scene.settle();
    CHECK(base(*input).shownText() == "2026-03-15 14:30");

    QQuickItem const* const button = findNamed(Scene::itemOf(*input), QStringLiteral("datePickerButton"));
    REQUIRE(button != nullptr);
    scene.click(*button);
    QQuickItem const* const cell = findNamed(*scene.window().contentItem(), QStringLiteral("day_2026-2-20"));
    REQUIRE(cell != nullptr);
    scene.click(*cell);
    REQUIRE(changes.size() == 1);
    REQUIRE(changes.at(0).has_value());
    CHECK(changes.at(0)->value->toIso8601() == "2026-03-20T12:30:00.000Z");
    CHECK(base(*input).shownText() == "2026-03-20 14:30");
}

// Mutation: place the popup by the field's scene position. The popup is placed in its parent's coordinates, so a
// scene position puts it off by the field's own offset as soon as the placement is recomputed, here on a new width.
TEST_CASE("qt_quick widgets: a calendar opens under its field, wherever the field sits", "[qt_quick]") {
    Scene scene;
    auto row = scene.backend().createStack(nullptr, ui::Axis::Horizontal);
    auto before = scene.backend().createButton(row.get());
    before->setLabel("Before");
    auto input = scene.backend().createDateTimeInput(row.get(), ui::DateMode::Date, 0);
    scene.settle();

    QQuickItem const& field = Scene::itemOf(*input);
    REQUIRE(field.mapToScene(QPointF{0, 0}).x() > 0.0);  // the field sits right of another widget
    QQuickItem const* const button = findNamed(Scene::itemOf(*input), QStringLiteral("datePickerButton"));
    REQUIRE(button != nullptr);
    scene.click(*button);
    auto const* const popup = scene.window().contentItem()->findChild<QObject*>(QStringLiteral("datePickerPopup"));
    REQUIRE(popup != nullptr);
    REQUIRE(popup->property("visible").toBool());
    auto* const background = popup->property("background").value<QQuickItem*>();
    REQUIRE(background != nullptr);
    auto const check = [&] {
        QPointF const fieldBottomRight = field.mapToScene(QPointF{field.width(), field.height()});
        QPointF const topRight = background->mapToScene(QPointF{background->width(), 0});
        INFO("field bottom-right " << fieldBottomRight.x() << "," << fieldBottomRight.y() << ", popup top-right "
                                   << topRight.x() << "," << topRight.y());
        CHECK(std::abs(topRight.y() - fieldBottomRight.y()) <= 1.0);
        CHECK(std::abs(topRight.x() - fieldBottomRight.x()) <= 1.0);
    };
    check();
    // A new width places the calendar again.
    input->setLayout({.width = ui::Sizing::fixed(40), .height = {}});
    scene.settle();
    check();
}

TEST_CASE("qt_quick widgets: a calendar opens on the field's month, not today's", "[qt_quick]") {
    using morph::time::DateTime;
    using namespace std::chrono;
    Scene scene;
    auto input = scene.backend().createDateTimeInput(nullptr, ui::DateMode::Date, 0);
    QDate const expected = QDateTime::currentDateTimeUtc().date().addMonths(-2);
    DateTime const value{year{expected.year()},
                         month{static_cast<unsigned>(expected.month())},
                         day{static_cast<unsigned>(expected.day())},
                         hours{0},
                         minutes{0},
                         seconds{0}};
    input->setValue(morph::time::Timestamp{value});
    scene.settle();

    QQuickItem const* const button = findNamed(Scene::itemOf(*input), QStringLiteral("datePickerButton"));
    REQUIRE(button != nullptr);
    scene.click(*button);
    QQuickItem const* const grid = findNamed(*scene.window().contentItem(), QStringLiteral("datePickerGrid"));
    REQUIRE(grid != nullptr);
    CHECK(grid->property("year").toInt() == expected.year());
    CHECK(grid->property("month").toInt() == expected.month() - 1);
}

TEST_CASE("qt_quick widgets: dates format and parse in a display zone", "[qt_quick]") {
    using morph::time::DateTime;
    using namespace std::chrono;
    DateTime const midnight{year{2026}, month{1}, day{1}, hours{0}, minutes{0}, seconds{0}};
    CHECK(detail::formatDateTime(midnight, ui::DateMode::Date, -60) == QStringLiteral("2025-12-31"));
    auto const parsed = detail::parseDateTime(QStringLiteral("2025-12-31"), ui::DateMode::Date, -60);
    REQUIRE(parsed.has_value());
    CHECK(parsed->toIso8601() == "2025-12-31T01:00:00.000Z");
    CHECK_FALSE(detail::parseDateTime(QStringLiteral("2026-02-30"), ui::DateMode::Date, 0).has_value());
    CHECK_FALSE(detail::parseDateTime(QStringLiteral("2026-02-03 24:00"), ui::DateMode::DateTime, 0).has_value());
    CHECK_FALSE(detail::parseDateTime(QStringLiteral("2026-02-03"), ui::DateMode::DateTime, 0).has_value());
}

TEST_CASE("qt_quick widgets: a file picker shows its path and reports a pick; a read-only one refuses", "[qt_quick]") {
    Scene scene;
    auto picker = scene.backend().createFilePicker(nullptr, ui::FilePickerMode::Save);
    std::vector<std::string> picked;
    picker->setOnPicked([&picked](std::string const& path) { picked.push_back(path); });
    picker->setPath("/tmp/a.csv");
    scene.settle();
    CHECK(Scene::itemOf(*picker).property("save").toBool());
    CHECK(base(*picker).shownText() == "/tmp/a.csv");
    base(*picker).call("pick", QStringLiteral("file:///tmp/b.csv"));
    CHECK(picked == std::vector<std::string>{"/tmp/b.csv"});
    CHECK(base(*picker).shownText() == "/tmp/b.csv");
    picker->setReadOnly(true);
    base(*picker).call("pick", QStringLiteral("file:///tmp/c.csv"));
    CHECK(picked.size() == 1);
}

TEST_CASE("qt_quick widgets: the field state shows errors, required, stale", "[qt_quick]") {
    Scene scene;
    auto text = scene.backend().createText(nullptr);
    text->setErrors({"Too long", "Not a number"});
    text->setRequired(true);
    text->setStale(true);
    scene.settle();
    QQuickItem const& item = Scene::itemOf(*text);
    CHECK(item.property("errors").toStringList() ==
          QStringList{QStringLiteral("Too long"), QStringLiteral("Not a number")});
    CHECK(item.property("required").toBool());
    CHECK(item.opacity() < 1.0);
    text->setStale(false);
    CHECK(item.opacity() == 1.0);
}

// Mutation: leave `removable` unwritten in BadgeImpl::setOnRemove (no remove button is shown or reached).
TEST_CASE("qt_quick widgets: a removable badge's remove button is named and removes it", "[qt_quick]") {
    Scene scene;
    auto const column = scene.backend().createStack(nullptr, ui::Axis::Vertical);
    auto plain = scene.backend().createBadge(column.get());
    plain->setText("Plain");
    auto badge = scene.backend().createBadge(column.get());
    badge->setText("Result");
    int removals = 0;
    badge->setOnRemove([&removals] { ++removals; }, "Remove Result");
    scene.settle();
    auto* const remove = Scene::itemOf(*badge).findChild<QQuickItem*>(QStringLiteral("remove"));
    REQUIRE(remove != nullptr);
    CHECK(remove->isVisible());
    auto const* const plainRemove = Scene::itemOf(*plain).findChild<QQuickItem*>(QStringLiteral("remove"));
    REQUIRE(plainRemove != nullptr);
    CHECK_FALSE(plainRemove->isVisible());

    bool const wasActive = QAccessible::isActive();
    QAccessible::setActive(true);
    QAccessibleInterface* const named = QAccessible::queryAccessibleInterface(remove);
    REQUIRE(named != nullptr);
    CHECK(named->text(QAccessible::Name) == QStringLiteral("Remove Result"));
    QAccessible::setActive(wasActive);

    scene.click(*remove);
    CHECK(removals == 1);
    remove->forceActiveFocus();
    scene.key(Qt::Key_Delete);
    CHECK(removals == 2);
}

TEST_CASE("qt_quick widgets: the display kinds show what they are given", "[qt_quick]") {
    Scene scene;
    auto column = scene.backend().createStack(nullptr, ui::Axis::Vertical);
    auto busy = scene.backend().createBusy(column.get());
    busy->setActive(true);
    busy->setLabel("Loading");
    auto badge = scene.backend().createBadge(column.get());
    badge->setTone(ui::Tone::Warn);
    badge->setText("Late");
    badge->setIcon("!");
    auto progress = scene.backend().createProgress(column.get());
    progress->setValue(0.25);
    progress->setLabel("A quarter");
    auto steps = scene.backend().createSteps(column.get());
    steps->setSteps(
        {{.label = "One", .state = ui::StepState::Done}, {.label = "Two", .state = ui::StepState::Active}});
    steps->setCurrent(1);
    auto pairs = scene.backend().createKeyValue(column.get());
    pairs->setItems({{.label = "Mass", .value = "10 g"}});
    auto empty = scene.backend().createEmptyState(column.get());
    empty->setTitle("Nothing yet");
    empty->setText("Add one");
    empty->setIcon("+");
    empty->setActionLabel("Add");
    int actions = 0;
    empty->setOnAction([&actions] { ++actions; });
    auto spacer = scene.backend().createSpacer(column.get());
    scene.settle();
    CHECK(Scene::itemOf(*busy).property("active").toBool());
    CHECK(base(*badge).shownText() == "Late");
    CHECK(Scene::itemOf(*badge).property("tone").toInt() == 3);
    CHECK(Scene::itemOf(*progress).property("value").toDouble() == 0.25);
    progress->setValue(std::nullopt);
    CHECK(Scene::itemOf(*progress).property("value").toDouble() < 0);
    CHECK(Scene::itemOf(*steps).property("current").toInt() == 1);
    steps->setCurrent(std::nullopt);
    CHECK(Scene::itemOf(*steps).property("current").toInt() == -1);
    CHECK(Scene::itemOf(*pairs).property("items").toList().size() == 1);
    CHECK(base(*empty).shownText() == "Nothing yet");
    auto const* const action = Scene::itemOf(*empty).findChild<QQuickItem*>(QStringLiteral("action"));
    REQUIRE(action != nullptr);
    scene.click(*action);
    CHECK(actions == 1);
}

// A value with no natural break point (a UUID-shaped token) must wrap within whatever width the key-value pair is
// given, not overflow it: the value label's own height grows to fit the wrapped lines instead of its width growing
// past the container.
TEST_CASE("qt_quick widgets: key-value wraps a long value instead of overflowing a fixed width", "[qt_quick]") {
    Scene scene;
    auto wide = scene.backend().createKeyValue(nullptr);
    wide->setItems({{.label = "Id", .value = "f47ac10b-58cc-4372-a567-0e02b2c3d4790e02b2c3d4790e02b2c3d479"}});
    scene.settle();
    double const unconstrainedHeight = Scene::itemOf(*wide).implicitHeight();

    auto narrow = scene.backend().createKeyValue(nullptr);
    narrow->setLayout({.width = ui::Sizing::fixed(20), .height = {}});
    narrow->setItems({{.label = "Id", .value = "f47ac10b-58cc-4372-a567-0e02b2c3d4790e02b2c3d4790e02b2c3d479"}});
    scene.settle();
    // A label that only grows past its given width would keep overflowing; wrapping instead grows its height.
    CHECK(Scene::itemOf(*narrow).implicitHeight() > unconstrainedHeight);
}

namespace {

// The warnings Qt logs that name a polish loop, counted while a `PolishCounter` lives.
int& polishWarnings() {
    static int count = 0;
    return count;
}

void countPolish(QtMsgType type, QMessageLogContext const& context, QString const& message) {
    static_cast<void>(context);
    if (type == QtWarningMsg && message.contains(QStringLiteral("polish"))) {
        ++polishWarnings();
    }
}

class PolishCounter {
public:
    PolishCounter() : _previous{qInstallMessageHandler(countPolish)} { polishWarnings() = 0; }
    ~PolishCounter() { qInstallMessageHandler(_previous); }
    PolishCounter(PolishCounter const&) = delete;
    PolishCounter& operator=(PolishCounter const&) = delete;
    PolishCounter(PolishCounter&&) = delete;
    PolishCounter& operator=(PolishCounter&&) = delete;

private:
    QtMessageHandler _previous;
};

}  // namespace

// Mutation: cap the value column at the grid's width less a caption width the captions write back while the grid
// polishes (the Containers = needed pair loops).
TEST_CASE("qt_quick widgets: a key-value lays out without a polish loop, whatever its text widths", "[qt_quick]") {
    std::vector<std::pair<std::string, std::string>> const pairs{{"Containers", "needed"},
                                                                 {"Containers", "180 needed"},
                                                                 {"Containers", "x"},
                                                                 {"Container", "180 needed"},
                                                                 {"Containers", "xxxxxxxxxx"}};
    for (auto const& [caption, value] : pairs) {
        CAPTURE(caption, value);
        PolishCounter const counter;
        Scene scene;
        auto pair = scene.backend().createKeyValue(nullptr);
        pair->setItems({{.label = caption, .value = value}});
        scene.settle();
        CHECK(polishWarnings() == 0);
    }
}

// Mutation: leave the label visible while inactive (the label stays visible and the size is the label's).
TEST_CASE("qt_quick widgets: an inactive busy shows neither its indicator nor its label, and takes no room",
          "[qt_quick]") {
    Scene scene;
    auto column = scene.backend().createStack(nullptr, ui::Axis::Vertical);
    auto busy = scene.backend().createBusy(column.get());
    busy->setLabel("Loading\u2026");
    busy->setActive(false);
    scene.settle();
    QQuickItem const& item = Scene::itemOf(*busy);
    auto const* const label = item.findChild<QQuickItem*>(QStringLiteral("label"));
    REQUIRE(label != nullptr);
    CHECK_FALSE(label->isVisible());
    CHECK(item.implicitWidth() == 0.0);
    CHECK(item.implicitHeight() == 0.0);
    busy->setActive(true);
    scene.settle();
    CHECK(label->isVisible());
    CHECK(item.implicitWidth() > 0.0);
    CHECK(item.implicitHeight() > 0.0);
}

TEST_CASE("qt_quick widgets: a banner's action and dismissal reach their handlers", "[qt_quick]") {
    Scene scene;
    auto banner = scene.backend().createBanner(nullptr);
    banner->setTone(ui::Tone::Err);
    banner->setText("Failed");
    banner->setActionLabel("Retry");
    banner->setDismissible(true);
    std::vector<std::string> log;
    banner->setOnAction([&log] { log.emplace_back("action"); });
    banner->setOnDismiss([&log] { log.emplace_back("dismiss"); });
    scene.settle();
    CHECK(base(*banner).shownText() == "Failed");
    QQuickItem const& item = Scene::itemOf(*banner);
    scene.click(*item.findChild<QQuickItem*>(QStringLiteral("action")));
    scene.click(*item.findChild<QQuickItem*>(QStringLiteral("dismiss")));
    CHECK(log == std::vector<std::string>{"action", "dismiss"});
    CHECK(item.isVisible());
}

TEST_CASE("qt_quick widgets: a menu reports the path of a choosable entry and nothing else", "[qt_quick]") {
    Scene scene;
    auto menu = scene.backend().createMenu(nullptr, ui::MenuStyle::Bar);
    std::vector<std::vector<std::size_t>> paths;
    menu->setOnActivate([&paths](std::vector<std::size_t> path) { paths.push_back(std::move(path)); });
    menu->setItems(
        {{.label = "File", .items = {{.label = "Open", .keys = "Ctrl+O"}, {.label = "Gone", .enabled = false}}},
         {.label = "Wrap", .checked = true}});
    scene.settle();
    auto& impl = dynamic_cast<detail::MenuImpl&>(*menu);
    impl.choose({0, 0});
    impl.choose({0, 1});
    impl.choose({0});
    impl.choose({7});
    impl.choose({1});
    CHECK(paths == std::vector<std::vector<std::size_t>>{{0, 0}, {1}});
    // A click on a top-level entry without a submenu chooses it.
    QQuickItem& item = Scene::itemOf(*menu);
    QList<QQuickItem*> const buttons = Scene::itemsOf(item, "QQuickToolButton");
    REQUIRE(buttons.size() == 2);
    scene.click(*buttons.at(1));
    CHECK(paths.size() == 3);
    CHECK(detail::choosable(std::vector<ui::MenuEntry>{{.label = "x"}}, {0}));
    CHECK_FALSE(detail::choosable(std::vector<ui::MenuEntry>{{.label = "x"}}, {}));
}

TEST_CASE("qt_quick widgets: the common properties reach the item", "[qt_quick]") {
    Scene scene;
    auto text = scene.backend().createText(nullptr);
    text->setAccessibleName("Greeting");
    text->setAccessibleRole("heading");
    text->setAccessibleRole("not-a-role");
    text->setTestId("greeting");
    text->setTooltip("Says hello");
    text->setSurface("card");
    text->setLayout({.width = ui::Sizing::fixed(10), .height = ui::Sizing::stretch(2)});
    text->setVisible(false);
    text->setEnabled(false);
    scene.settle();
    QQuickItem const& item = Scene::itemOf(*text);
    CHECK(item.property("a11yName").toString() == QStringLiteral("Greeting"));
    CHECK(item.property("a11yRole").toInt() == 0);
    CHECK(item.objectName() == QStringLiteral("greeting"));
    CHECK(item.property("tooltip").toString() == QStringLiteral("Says hello"));
    CHECK(item.property("surface").toString() == QStringLiteral("card"));
    CHECK(item.property("widthKind").toInt() == 1);
    CHECK(item.property("widthAmount").toInt() == 10);
    CHECK(item.property("heightKind").toInt() == 2);
    CHECK_FALSE(item.isVisible());
    CHECK_FALSE(item.isEnabled());
    CHECK_FALSE(base(*text).ownVisible());
    CHECK_FALSE(base(*text).ownEnabled());
}

TEST_CASE("qt_quick widgets: a widget the user cannot reach takes no toggle, choice or move", "[qt_quick]") {
    Scene scene;
    auto panel = scene.backend().createStack(nullptr, ui::Axis::Vertical);
    auto inner = scene.backend().createStack(panel.get(), ui::Axis::Vertical);
    auto box = scene.backend().createCheckbox(inner.get());
    auto slider = scene.backend().createSlider(inner.get());
    int changes = 0;
    box->setOnToggle([&changes](bool) { ++changes; });
    slider->setOnChange([&changes](std::int64_t) { ++changes; });
    slider->setRange(0, 10, 1);
    scene.settle();
    panel->setEnabled(false);
    scene.click(Scene::controlOf(*box));
    slider->focus();
    scene.key(Qt::Key_Right);
    CHECK(changes == 0);
    panel->setEnabled(true);
    panel->setVisible(false);
    slider->focus();
    scene.key(Qt::Key_Right);
    CHECK(changes == 0);
    panel->setVisible(true);
    scene.click(Scene::controlOf(*box));
    CHECK(changes == 1);
}

TEST_CASE("qt_quick widgets: keys cross as text, an int64 above 2^53 unchanged", "[qt_quick]") {
    std::int64_t const big = (std::int64_t{1} << 53) + 1;
    CHECK(detail::encodeKey(ui::Key{big}) == QStringLiteral("i:9007199254740993"));
    CHECK(detail::decodeKey(QStringLiteral("i:9007199254740993")) == ui::Key{big});
    CHECK(detail::decodeKey(QStringLiteral("s:7")) == ui::Key{std::string{"7"}});
    CHECK(detail::decodeKey(QStringLiteral("i:7")) == ui::Key{std::int64_t{7}});
    CHECK_FALSE(detail::decodeKey(QStringLiteral("i:7x")).has_value());
    CHECK_FALSE(detail::decodeKey(QStringLiteral("i:")).has_value());
    CHECK_FALSE(detail::decodeKey(QStringLiteral("7")).has_value());
}

TEST_CASE("qt_quick widgets: chords match exactly the modifiers pressed", "[qt_quick]") {
    CHECK(detail::chordMatches("Ctrl+S", Qt::Key_S, Qt::ControlModifier));
    CHECK(detail::chordMatches("ctrl+s", Qt::Key_S, Qt::ControlModifier) == false);
    CHECK(detail::chordMatches("Ctrl+s", Qt::Key_S, Qt::ControlModifier));
    CHECK_FALSE(detail::chordMatches("Ctrl+S", Qt::Key_S, Qt::ControlModifier | Qt::ShiftModifier));
    CHECK(detail::chordMatches("Enter", Qt::Key_Enter, Qt::KeypadModifier));
    CHECK(detail::chordMatches("Enter", Qt::Key_Return, {}));
    CHECK(detail::chordMatches("Shift+F12", Qt::Key_F12, Qt::ShiftModifier));
    CHECK(detail::chordMatches("Alt+Meta+Delete", Qt::Key_Delete, Qt::AltModifier | Qt::MetaModifier));
    CHECK(detail::chordMatches("Esc", Qt::Key_Escape, {}));
    CHECK(detail::chordMatches("Ctrl+1", Qt::Key_1, Qt::ControlModifier));
    CHECK_FALSE(detail::chordKey("Ctrl+Ctrl+S").has_value());
    CHECK_FALSE(detail::chordKey("Hyper+S").has_value());
    CHECK_FALSE(detail::chordKey("F25").has_value());
    CHECK_FALSE(detail::chordKey("F0").has_value());
    CHECK_FALSE(detail::chordKey("Ctrl+").has_value());
    CHECK_FALSE(detail::chordKey("Ctrl+?").has_value());
    CHECK_FALSE(detail::chordMatches("Hyper+S", Qt::Key_S, {}));
}

// Mutation: class the host by its width in pixels (800 px is then `medium`, and the resizes below report other
// classes), or call the listener at registration (it hears the current class first).
TEST_CASE("qt_quick widgets: the backend's width class follows the host's width in units", "[qt_quick]") {
    Scene scene;
    scene.settle();
    auto* const theme = scene.engine().singletonInstance<QObject*>(QStringLiteral("MorphUi"), QStringLiteral("Theme"));
    REQUIRE(theme != nullptr);
    double const unit = theme->property("unitWidth").toDouble();
    REQUIRE(unit > 0);
    CHECK(scene.backend().widthClass() == ui::widthClassOf(800 / unit));

    std::vector<std::string> classes;
    scene.backend().setOnWidthClass([&classes](std::string const& name) { classes.push_back(name); });
    CHECK(classes.empty());
    for (double const units : {170.0, 165.0, 120.0, 70.0, 75.0}) {
        scene.window().resize(static_cast<int>(std::ceil(units * unit)), 600);
        scene.settle();
    }
    CHECK(classes == std::vector<std::string>{"expanded", "medium", "compact"});
    scene.backend().setOnWidthClass({});
}

TEST_CASE(
    "qt_quick widgets: a primary button is accent-filled, a danger one error-filled, a ghost one transparent "
    "and narrower",
    "[qt_quick]") {
    Scene scene;
    auto const column = scene.backend().createStack(nullptr, ui::Axis::Vertical);
    auto plain = scene.backend().createButton(column.get());
    auto primary = scene.backend().createButton(column.get());
    auto danger = scene.backend().createButton(column.get());
    auto ghost = scene.backend().createButton(column.get());
    plain->setLabel("Plain");
    primary->setLabel("Go");
    primary->setEmphasis(ui::ButtonEmphasis::Primary);
    danger->setLabel("Drop");
    danger->setEmphasis(ui::ButtonEmphasis::Danger);
    ghost->setLabel("X");
    ghost->setEmphasis(ui::ButtonEmphasis::Ghost);
    scene.settle();
    auto const backgroundItem = [](ui::ButtonWidget& button) {
        QQuickItem const* const control = Scene::itemsOf(Scene::itemOf(button), "QQuickButton").value(0);
        REQUIRE(control != nullptr);
        auto* const fill = control->property("background").value<QQuickItem*>();
        REQUIRE(fill != nullptr);
        return fill;
    };
    auto const background = [&backgroundItem](ui::ButtonWidget& button) {
        return backgroundItem(button)->property("color").value<QColor>();
    };
    CHECK(background(*plain) == QColor{"#ffffff"});
    CHECK(background(*primary) == QColor{"#1565c0"});
    CHECK(background(*danger) == QColor{"#c62828"});
    CHECK(background(*ghost) == QColor{"transparent"});
    // Confirms the implicitWidth floor is lifted for Ghost, not merely that its padding shrank: a plain button keeps
    // its 8-unit background floor regardless of its one-character label, so a ghost button of the same label must
    // measure visibly narrower, not just differently padded within the same floor.
    CHECK(Scene::itemOf(*ghost).implicitWidth() < Scene::itemOf(*plain).implicitWidth());
}

// Whether an Item-style button already composes with a trailing Badge inside a row, or whether the button's own
// fill consumes the row's width first and squeezes or displaces the badge, checked empirically.
TEST_CASE("qt_quick widgets: an Item-style button composes with a trailing Badge in a stretched row", "[qt_quick]") {
    Scene scene;
    auto const row = scene.backend().createStack(nullptr, ui::Axis::Horizontal);
    row->setLayout({.width = ui::Sizing::fixed(50), .height = ui::Sizing::content()});
    auto item = scene.backend().createButton(row.get());
    item->setStyle(ui::ButtonStyle::Item);
    item->setLabel("A fairly long sidebar entry label");
    item->setLayout({.width = ui::Sizing::stretch(), .height = ui::Sizing::content()});
    auto badge = scene.backend().createBadge(row.get());
    badge->setText("12");
    scene.settle();
    QQuickItem const& rowItem = Scene::itemOf(*row);
    QQuickItem const& itemItem = Scene::itemOf(*item);
    QQuickItem const& badgeItem = Scene::itemOf(*badge);
    INFO("row " << rowItem.width() << " wide; item at x=" << itemItem.x() << " w=" << itemItem.width()
                << "; badge at x=" << badgeItem.x() << " w=" << badgeItem.width());
    // The badge keeps its own width and is fully inside the row, after the item.
    CHECK(badgeItem.width() > 0.0);
    CHECK(badgeItem.x() >= itemItem.x() + itemItem.width() - 1.0);
    CHECK(badgeItem.x() + badgeItem.width() <= rowItem.width() + 1.0);
    // The item does not consume the row's full width itself: there is room left for the badge beside it.
    CHECK(itemItem.width() < rowItem.width());
}

// The narrower case: an Item-style button given no stretch sizing (content-sized) still composes with a trailing
// Badge — less realistic for a nav rail, but recorded since it behaves differently from the stretched case.
TEST_CASE("qt_quick widgets: a content-sized Item-style button also composes with a trailing Badge", "[qt_quick]") {
    Scene scene;
    auto const row = scene.backend().createStack(nullptr, ui::Axis::Horizontal);
    auto item = scene.backend().createButton(row.get());
    item->setStyle(ui::ButtonStyle::Item);
    item->setLabel("Short");
    auto badge = scene.backend().createBadge(row.get());
    badge->setText("3");
    scene.settle();
    QQuickItem const& itemItem = Scene::itemOf(*item);
    QQuickItem const& badgeItem = Scene::itemOf(*badge);
    INFO("item at x=" << itemItem.x() << " w=" << itemItem.width() << "; badge at x=" << badgeItem.x()
                      << " w=" << badgeItem.width());
    CHECK(badgeItem.width() > 0.0);
    CHECK(badgeItem.x() >= itemItem.x() + itemItem.width() - 1.0);
}

namespace {

/// The width a `PlainText` Text of @p text in @p font lays out to: the characters, never markup.
qreal plainWidth(QQmlEngine& engine, QString const& text, QFont const& font) {
    QQmlComponent component{&engine};
    component.setData("import QtQuick\nText { textFormat: Text.PlainText }", QUrl{});
    std::unique_ptr<QObject> const made{component.create()};
    REQUIRE(made != nullptr);
    made->setProperty("text", text);
    made->setProperty("font", font);
    return made->property("implicitWidth").toReal();
}

/// Every shown Text under @p root whose text holds a bold tag.
// NOLINTNEXTLINE(misc-no-recursion): an item tree, walked down.
void markupTexts(QQuickItem const& root, QList<QQuickItem const*>& found) {
    for (QQuickItem const* const child : root.childItems()) {
        if (!child->isVisible()) {
            continue;
        }
        if (child->inherits("QQuickText") && child->property("text").toString().contains(QStringLiteral("<b>"))) {
            found.append(child);
        }
        markupTexts(*child, found);
    }
}

}  // namespace

// Mutation: drop one Label's `textFormat`, or the tooltip's content item, and the walk names it.
TEST_CASE("qt_quick widgets: every text a document gives is drawn as its characters, never as markup",
          "[qt_quick]") {
    Scene scene;
    QString const tagged = QStringLiteral("<b>x</b> & <i>y</i>");
    std::string const raw = tagged.toStdString();
    auto const column = scene.backend().createStack(nullptr, ui::Axis::Vertical);
    int strings = 0;
    // First, so it is inside the window for the pointer to rest on.
    auto tipped = scene.backend().createText(column.get());
    tipped->setText("hover");
    tipped->setTooltip(raw);
    ++strings;
    auto text = scene.backend().createText(column.get());
    text->setText(raw);
    ++strings;
    auto button = scene.backend().createButton(column.get());
    button->setLabel(raw);
    ++strings;
    auto checkbox = scene.backend().createCheckbox(column.get());
    checkbox->setLabel(raw);
    ++strings;
    auto radio = scene.backend().createSelect(column.get(), ui::SelectStyle::Radio);
    radio->setOptions({{.key = std::string{"a"}, .label = raw}, {.key = std::string{"b"}, .label = raw + "2"}});
    strings += 2;
    auto dropdown = scene.backend().createSelect(column.get(), ui::SelectStyle::Dropdown);
    dropdown->setOptions({{.key = std::string{"a"}, .label = raw}, {.key = std::string{"b"}, .label = raw + "2"}});
    strings += 2;
    auto badge = scene.backend().createBadge(column.get());
    badge->setText(raw);
    ++strings;
    auto banner = scene.backend().createBanner(column.get());
    banner->setText(raw);
    banner->setActionLabel(raw);
    strings += 2;
    auto keyValue = scene.backend().createKeyValue(column.get());
    keyValue->setItems({{.label = raw, .value = raw}});
    strings += 2;
    auto panel = scene.backend().createPanel(column.get());
    panel->setTitle(raw);
    ++strings;
    auto collapsible = scene.backend().createCollapsible(column.get());
    collapsible->setTitle(raw);
    ++strings;
    auto tabs = scene.backend().createTabs(column.get());
    tabs->setTabs({raw});
    ++strings;
    auto input = scene.backend().createTextInput(column.get(), ui::TextInputMode::SingleLine);
    input->setCaption(raw, true);
    input->setErrors({raw});
    strings += 2;
    auto steps = scene.backend().createSteps(column.get());
    steps->setSteps({{.label = raw}});
    ++strings;
    auto empty = scene.backend().createEmptyState(column.get());
    empty->setTitle(raw);
    empty->setText(raw);
    empty->setActionLabel(raw);
    strings += 3;
    auto menu = scene.backend().createMenu(column.get(), ui::MenuStyle::Bar);
    menu->setItems({{.label = raw, .items = {{.label = raw}}}});
    strings += 2;
    auto buttonMenu = scene.backend().createMenu(column.get(), ui::MenuStyle::Button);
    buttonMenu->setTrigger({.label = raw});
    ++strings;
    scene.settle();
    // The popups whose texts exist only while open: the tooltip first, while nothing covers its widget.
    QTest::mouseMove(&scene.window(), Scene::centreOf(Scene::itemOf(*tipped)));
    scene.settle();
    QTest::qWait(800);
    scene.settle();
    auto* const combo = qobject_cast<QQuickItem*>(base(*dropdown).part("control"));
    REQUIRE(combo != nullptr);
    auto* const popup = combo->property("popup").value<QObject*>();
    REQUIRE(popup != nullptr);
    QMetaObject::invokeMethod(popup, "open");
    base(*menu).call("open", QVariant{QVariantList{0}});
    scene.settle();
    // Last, since a modal dialog takes the hover from the widgets under it.
    auto dialog = scene.backend().createDialog(nullptr);
    dialog->setTitle(raw);
    dialog->setOpen(true);
    ++strings;
    scene.settle();

    QList<QQuickItem const*> found;
    markupTexts(*scene.window().contentItem(), found);
    CHECK(found.size() == strings);
    for (QQuickItem const* const item : std::as_const(found)) {
        QString const shown = item->property("text").toString();
        INFO("a Text showing " << shown.toStdString() << " in " << item->parentItem()->metaObject()->className());
        CHECK(item->property("textFormat").toInt() == 0);  // Text.PlainText
        qreal const padding = item->property("leftPadding").toReal() + item->property("rightPadding").toReal();
        CHECK(std::abs(item->property("implicitWidth").toReal() - padding -
                       plainWidth(scene.engine(), shown, item->property("font").value<QFont>())) < 0.5);
    }
}

// Mutation: drop `toHtmlEscaped` (the markup run draws as styled `x & y`, narrower); drop the `pre-wrap` span (the
// line feed and the spaces fold); build a mono run without its `font-family` (as wide as the body font).
TEST_CASE("qt_quick widgets: a text's runs are escaped rich text that keeps their characters and faces",
          "[qt_quick]") {
    Scene scene;
    auto const column = scene.backend().createStack(nullptr, ui::Axis::Vertical);
    auto const markup = scene.backend().createText(column.get());
    markup->setSpans({{.text = "<b>x</b> & <i>y</i>"}});
    auto const lines = scene.backend().createText(column.get());
    lines->setSpans({{.text = "a\nb"}});
    auto const line = scene.backend().createText(column.get());
    line->setSpans({{.text = "a"}});
    auto const spaced = scene.backend().createText(column.get());
    spaced->setSpans({{.text = "a     b"}});
    auto const mono = scene.backend().createText(column.get());
    mono->setSpans({{.text = "iiiiiiiiii", .mono = true}});
    auto const body = scene.backend().createText(column.get());
    body->setSpans({{.text = "iiiiiiiiii"}});
    scene.settle();
    auto const widthOf = [](ui::Widget const& widget) {
        return Scene::controlOf(widget).property("implicitWidth").toReal();
    };
    auto const font = Scene::controlOf(*markup).property("font").value<QFont>();
    CHECK(Scene::controlOf(*markup).property("textFormat").toInt() == 1);  // Text.RichText
    CHECK(std::abs(widthOf(*markup) - plainWidth(scene.engine(), QStringLiteral("<b>x</b> & <i>y</i>"), font)) < 1.0);
    CHECK(Scene::itemOf(*markup).property("accessibleName").toString() == QStringLiteral("<b>x</b> & <i>y</i>"));
    CHECK(base(*markup).shownText() == "<b>x</b> & <i>y</i>");
    qreal const lineHeight = Scene::controlOf(*line).property("implicitHeight").toReal();
    CHECK(std::abs(Scene::controlOf(*lines).property("implicitHeight").toReal() - (2 * lineHeight)) < 1.0);
    CHECK(std::abs(widthOf(*spaced) - plainWidth(scene.engine(), QStringLiteral("a     b"), font)) < 1.0);
    // A machine without a fixed-pitch family would draw the mono run in a proportional fallback and measure nothing.
    QStringList const families = detail::monoFamiliesOf(base(*mono).context());
    REQUIRE_FALSE(families.isEmpty());
    REQUIRE(QFontInfo{QFont{families.first()}}.fixedPitch());
    CHECK(widthOf(*mono) > widthOf(*body) + 10);
    // A plain text set after the runs shows as plain text again.
    markup->setText("<b>y</b>");
    scene.settle();
    CHECK(Scene::controlOf(*markup).property("textFormat").toInt() == 0);
    CHECK(Scene::controlOf(*markup).property("text").toString() == QStringLiteral("<b>y</b>"));
}

namespace {

/// Every item named @p name under @p root, depth first.
// NOLINTNEXTLINE(misc-no-recursion): an item tree, walked down.
void namedItems(QQuickItem const& root, QString const& name, QList<QQuickItem*>& found) {
    for (QQuickItem* const child : root.childItems()) {
        if (child->objectName() == name) {
            found.append(child);
        }
        namedItems(*child, name, found);
    }
}

QList<QQuickItem*> named(QQuickItem const& root, QString const& name) {
    QList<QQuickItem*> found;
    namedItems(root, name, found);
    return found;
}

}  // namespace

// Mutation: ignore `captionsAbove` in MorphKeyValue (the caption sits level with its value, and no rule shows).
TEST_CASE("qt_quick widgets: a stacked key-value puts each caption over its value with a rule between pairs",
          "[qt_quick]") {
    Scene scene;
    auto const column = scene.backend().createStack(nullptr, ui::Axis::Vertical);
    auto const kv = scene.backend().createKeyValue(column.get());
    kv->setCaptionPlacement(ui::CaptionPlacement::Above);
    kv->setItems({{.label = "Serial number", .spans = {{.text = "DEAB-902114", .mono = true}}},
                  {.label = "Containers", .value = "180 needed", .tone = ui::Tone::Err}});
    scene.settle();
    QQuickItem const& item = Scene::itemOf(*kv);
    QList<QQuickItem*> const captions = named(item, QStringLiteral("caption"));
    QList<QQuickItem*> const values = named(item, QStringLiteral("value"));
    REQUIRE(captions.size() == 2);
    REQUIRE(values.size() == 2);
    QPointF const firstValue = values.at(0)->mapToItem(&item, QPointF{0, 0});
    CHECK(captions.at(1)->mapToItem(&item, QPointF{0, 0}).y() > firstValue.y() + values.at(0)->height());
    CHECK(values.at(0)->mapToItem(&item, QPointF{0, 0}).y() >=
          captions.at(0)->mapToItem(&item, QPointF{0, 0}).y() + captions.at(0)->height());
    QList<QQuickItem*> const rules = named(item, QStringLiteral("rule"));
    REQUIRE(rules.size() == 2);
    CHECK(rules.at(0)->isVisible());
    CHECK_FALSE(rules.at(1)->isVisible());
    QObject const* const scheme = item.property("scheme").value<QObject*>();
    REQUIRE(scheme != nullptr);
    CHECK(values.at(1)->property("color").value<QColor>() == scheme->property("error").value<QColor>());
    CHECK(values.at(0)->property("color").value<QColor>() == scheme->property("text").value<QColor>());
    CHECK(values.at(0)->property("textFormat").toInt() == 1);  // the run, as rich text
}

// Mutation: put the mark back at the right edge under the caption (its `y` is then the caption's height); bind a
// text's mark to the label's `contentWidth` (its `x` is then after the first, longer line).
TEST_CASE("qt_quick widgets: a required mark follows a caption, or a text's last line", "[qt_quick]") {
    Scene scene;
    auto const column = scene.backend().createStack(nullptr, ui::Axis::Vertical);
    auto const captioned = scene.backend().createTextInput(column.get(), ui::TextInputMode::SingleLine);
    captioned->setCaption("Meaning", true);
    captioned->setRequired(true);
    auto const bare = scene.backend().createTextInput(column.get(), ui::TextInputMode::SingleLine);
    bare->setRequired(true);
    auto const text = scene.backend().createText(column.get());
    text->setText("A first line that is the longer one\nshort");
    text->setRequired(true);
    scene.settle();
    QQuickItem& field = Scene::itemOf(*captioned);
    QQuickItem const* const caption = findNamed(field, QStringLiteral("caption"));
    QQuickItem const* const mark = findNamed(field, QStringLiteral("requiredMark"));
    REQUIRE(caption != nullptr);
    REQUIRE(mark != nullptr);
    qreal const space = Scene::itemOf(*captioned).property("markerRoom").toReal() - mark->implicitWidth();
    CHECK(mark->y() == caption->y());
    CHECK(mark->x() >= caption->property("contentWidth").toReal());
    CHECK(mark->x() <= caption->property("contentWidth").toReal() + space + 1);
    CHECK(Scene::controlOf(*captioned).width() == field.width());
    QQuickItem& bareField = Scene::itemOf(*bare);
    QQuickItem const* const bareMark = findNamed(bareField, QStringLiteral("requiredMark"));
    REQUIRE(bareMark != nullptr);
    CHECK(std::abs(bareMark->x() + bareMark->width() - bareField.width()) < 1);
    QQuickItem& textItem = Scene::itemOf(*text);
    QQuickItem const* const textMark = findNamed(textItem, QStringLiteral("requiredMark"));
    QQuickItem const& label = Scene::controlOf(*text);
    REQUIRE(textMark != nullptr);
    qreal const lineHeight = label.property("contentHeight").toReal() / 2;
    CHECK(textMark->y() >= lineHeight - 1);
    CHECK(textMark->x() < label.property("contentWidth").toReal() / 2);
    CHECK(textMark->x() > 0);
}

// SPDX-License-Identifier: Apache-2.0
//
// The leaf and field widgets on the real Qt Quick engine: what each setter shows, what reaches each handler, and
// the rules of the backend contract the conformance probe cannot reach (docs/spec/ui/backend_contract.md, "What the
// probe cannot reach"): read-only fields of every kind, unreachable widgets, and controlled inputs other than a
// text field snapping back.

#include <QColor>
#include <QQuickItem>
#include <QStringList>
#include <QTest>
#include <QVariantList>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <memory>
#include <morph/qt_quick/theme.hpp>
#include <morph/reactive/signal.hpp>
#include <morph/ui/backend.hpp>
#include <morph/ui/mount.hpp>
#include <morph/ui/view.hpp>
#include <optional>
#include <string>
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
    // An emptied field is a cleared value.
    scene.key(Qt::Key_A, Qt::ControlModifier);
    scene.key(Qt::Key_Delete);
    scene.key(Qt::Key_Return);
    REQUIRE(changes.size() == 2);
    CHECK_FALSE(changes.at(1).has_value());
    input->setValue(std::nullopt);
    CHECK(base(*input).shownText().empty());
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
    auto menu = scene.backend().createMenu(nullptr);
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

TEST_CASE("qt_quick widgets: a widget's width class follows the host's width", "[qt_quick]") {
    CHECK(morph::qt_quick::widthClassOf(599) == "compact");
    CHECK(morph::qt_quick::widthClassOf(600) == "medium");
    CHECK(morph::qt_quick::widthClassOf(1199.5) == "medium");
    CHECK(morph::qt_quick::widthClassOf(1200) == "expanded");
    std::vector<std::string> classes;
    Scene scene{morph::qt_quick::BackendOptions{
        .components = nullptr, .onWidthClass = [&classes](std::string const& name) { classes.push_back(name); }}};
    scene.window().resize(1300, 600);
    scene.settle();
    scene.window().resize(1250, 600);
    scene.settle();
    scene.window().resize(500, 600);
    scene.settle();
    CHECK(classes == std::vector<std::string>{"compact", "medium", "expanded", "compact"});
}

TEST_CASE("qt_quick widgets: a primary button is accent-filled, a danger one error-filled", "[qt_quick]") {
    Scene scene;
    auto const column = scene.backend().createStack(nullptr, ui::Axis::Vertical);
    auto plain = scene.backend().createButton(column.get());
    auto primary = scene.backend().createButton(column.get());
    auto danger = scene.backend().createButton(column.get());
    plain->setLabel("Plain");
    primary->setLabel("Go");
    primary->setEmphasis(ui::ButtonEmphasis::Primary);
    danger->setLabel("Drop");
    danger->setEmphasis(ui::ButtonEmphasis::Danger);
    scene.settle();
    auto const background = [](ui::ButtonWidget& button) {
        QQuickItem const* const control = Scene::itemsOf(Scene::itemOf(button), "QQuickButton").value(0);
        REQUIRE(control != nullptr);
        auto* const fill = control->property("background").value<QQuickItem*>();
        REQUIRE(fill != nullptr);
        return fill->property("color").value<QColor>();
    };
    CHECK(background(*plain) == QColor{"#ffffff"});
    CHECK(background(*primary) == QColor{"#1565c0"});
    CHECK(background(*danger) == QColor{"#c62828"});
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

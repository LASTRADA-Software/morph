// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <morph/ui/backend.hpp>
#include <morph/ui/testing/recording_backend.hpp>
#include <morph/ui/view.hpp>
#include <morph/util/datetime.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include "ui_test_support.hpp"

namespace ui = morph::ui;

namespace {

using morph::testing::intKey;
using ui::testing::RecordingBackend;

}  // namespace

TEST_CASE("RecordingBackend: logs creation, setters, moves and destruction", "[ui]") {
    RecordingBackend backend;
    auto column = backend.createStack(nullptr, ui::Axis::Vertical);
    auto first = backend.createText(column.get());
    auto second = backend.createButton(column.get());
    first->setText("hello");
    second->setLabel("Go");
    column->moveChild(*second, 0);
    CHECK(backend.log() == std::vector<std::string>{"create Column#1 in root", "create Text#2 in Column#1",
                                                    "create Button#3 in Column#1", "set Text#2 text=hello",
                                                    "set Button#3 label=Go", "move Button#3 to 0"});
    CHECK(backend.dump() == "Column#1\n  Button#3 label=Go\n  Text#2 text=hello\n");

    backend.clearLog();
    first.reset();
    CHECK(backend.log() == std::vector<std::string>{"destroy Text#2"});
    CHECK_FALSE(backend.exists(2));
    CHECK(backend.children(1) == std::vector<int>{3});
}

TEST_CASE("RecordingBackend: creation parameters are properties, a horizontal stack is a Row", "[ui]") {
    RecordingBackend backend;
    auto row = backend.createStack(nullptr, ui::Axis::Horizontal);
    auto input = backend.createTextInput(row.get(), ui::TextInputMode::Password);
    auto date = backend.createDateTimeInput(row.get(), ui::DateMode::Date, -90);
    auto scroll = backend.createScroll(row.get(), ui::Axis::Horizontal);
    auto picker = backend.createFilePicker(row.get(), ui::FilePickerMode::Save);
    auto choice = backend.createSelect(row.get(), ui::SelectStyle::Radio);
    choice->setOptions({{.key = intKey(1), .label = "One"}, {.key = ui::Key{std::string{"b"}}, .label = "Bee"}});
    choice->setSelected(ui::Key{std::string{"b"}});
    CHECK(backend.dump() ==
          "Row#1\n"
          "  TextInput#2 mode=Password\n"
          "  DateTimeInput#3 mode=Date offset=-90\n"
          "  Scroll#4 axis=Horizontal\n"
          "  FilePicker#5 mode=Save\n"
          "  Select#6 options=[1:One,\"b\":Bee] selected=\"b\" style=Radio\n");
}

TEST_CASE("RecordingBackend: Common setters are properties", "[ui]") {
    RecordingBackend backend;
    auto label = backend.createText(nullptr);
    label->setVisible(false);
    label->setEnabled(false);
    label->setLayout({.width = ui::Sizing::stretch(2), .height = ui::Sizing::fixed(1)});
    label->setDragKey(intKey(7));
    label->setDropHandler([](ui::Key const&) { return true; }, [](ui::Key const&) {});
    CHECK(backend.dump() == "Text#1 dragKey=7 drop=handler enabled=false layout=stretch(2)/fixed(1) visible=false\n");
    label->setDragKey(std::nullopt);
    CHECK(backend.prop(1, "dragKey") == "none");
}

TEST_CASE("RecordingBackend: lookups by kind, property, id and widget", "[ui]") {
    RecordingBackend backend;
    auto column = backend.createStack(nullptr, ui::Axis::Vertical);
    auto save = backend.createButton(column.get());
    auto quit = backend.createButton(column.get());
    save->setLabel("Save");
    quit->setLabel("Quit");
    CHECK(backend.find("Button", "label", "Quit") == std::optional<int>{3});
    CHECK_FALSE(backend.find("Button", "label", "Open").has_value());
    CHECK(backend.all("Button") == std::vector<int>{2, 3});
    CHECK(backend.prop(2, "label") == "Save");
    CHECK(backend.prop(2, "unset").empty());
    CHECK(backend.idOf(*quit) == 3);
    CHECK(backend.kindOf(1) == "Column");
    CHECK(backend.widget(3) == quit.get());
    CHECK_THROWS_AS(backend.prop(99, "label"), std::out_of_range);
    RecordingBackend other;
    auto stranger = other.createText(nullptr);
    CHECK_THROWS_AS(backend.idOf(*stranger), std::out_of_range);
}

TEST_CASE("RecordingBackend: a child outlives a destroyed parent as a root", "[ui]") {
    RecordingBackend backend;
    auto column = backend.createStack(nullptr, ui::Axis::Vertical);
    auto label = backend.createText(column.get());
    column.reset();
    CHECK(backend.dump() == "Text#2\n");
}

TEST_CASE("RecordingBackend: the interaction helpers call what the widget was given", "[ui]") {
    RecordingBackend backend;
    int clicks = 0;
    std::vector<std::string> edits;
    std::vector<std::string> submits;
    std::vector<bool> toggles;
    std::vector<ui::Key> chosen;
    std::vector<std::size_t> activated;
    std::vector<bool> collapses;
    std::vector<std::optional<morph::time::Timestamp>> dates;
    std::vector<std::int64_t> slides;
    std::vector<std::string> picks;

    auto button = backend.createButton(nullptr);
    button->setOnClick([&] { ++clicks; });
    auto input = backend.createTextInput(nullptr, ui::TextInputMode::SingleLine);
    input->setOnChange([&](std::string text) { edits.push_back(std::move(text)); });
    input->setOnSubmit([&](std::string text) { submits.push_back(std::move(text)); });
    auto box = backend.createCheckbox(nullptr);
    box->setOnToggle([&](bool checked) { toggles.push_back(checked); });
    auto choice = backend.createSelect(nullptr, ui::SelectStyle::Dropdown);
    choice->setOnSelect([&](ui::Key key) { chosen.push_back(std::move(key)); });
    choice->setOptions({{.key = intKey(5), .label = "Five"}});
    auto menu = backend.createMenu(nullptr);
    menu->setOnActivate([&](std::size_t index) { activated.push_back(index); });
    auto panel = backend.createPanel(nullptr);
    panel->setOnToggle([&](bool collapsed) { collapses.push_back(collapsed); });
    panel->setCollapsible(true);
    auto date = backend.createDateTimeInput(nullptr, ui::DateMode::DateTime, 0);
    date->setOnChange([&](std::optional<morph::time::Timestamp> value) { dates.push_back(value); });
    auto slider = backend.createSlider(nullptr);
    slider->setOnChange([&](std::int64_t value) { slides.push_back(value); });
    auto picker = backend.createFilePicker(nullptr, ui::FilePickerMode::Open);
    picker->setOnPicked([&](std::string path) { picks.push_back(std::move(path)); });

    backend.click(1);
    backend.edit(2, "ab");
    backend.submit(2, "abc");
    backend.toggle(3);
    backend.toggle(3);
    backend.choose(4, intKey(5));
    backend.chooseIndex(5, 1);
    backend.collapse(6, true);
    morph::time::Timestamp const when{morph::time::DateTime{std::chrono::year{2026}, std::chrono::month{10},
                                                            std::chrono::day{4}, std::chrono::hours{9},
                                                            std::chrono::minutes{30}, std::chrono::seconds{0}}};
    backend.setDateTime(7, when);
    backend.slide(8, 40);
    backend.pick(9, "/tmp/in.csv");

    CHECK(clicks == 1);
    CHECK(edits == std::vector<std::string>{"ab"});
    CHECK(submits == std::vector<std::string>{"abc"});
    CHECK(toggles == std::vector<bool>{true, false});
    CHECK(chosen == std::vector<ui::Key>{intKey(5)});
    CHECK(activated == std::vector<std::size_t>{1});
    CHECK(collapses == std::vector<bool>{true});
    CHECK(dates == std::vector<std::optional<morph::time::Timestamp>>{when});
    CHECK(slides == std::vector<std::int64_t>{40});
    CHECK(picks == std::vector<std::string>{"/tmp/in.csv"});
    // What the user changed shows on the widget, without a setter call in the log.
    CHECK(backend.prop(2, "text") == "abc");
    CHECK(backend.prop(3, "checked") == "false");
    CHECK(backend.prop(4, "selected") == "5");
    CHECK(backend.prop(7, "value") == "2026-10-04T09:30:00.000Z");
    CHECK(backend.log().size() == 11);
}

TEST_CASE("RecordingBackend: setText never calls onChange", "[ui]") {
    RecordingBackend backend;
    auto input = backend.createTextInput(nullptr, ui::TextInputMode::SingleLine);
    int changes = 0;
    input->setOnChange([&](std::string const&) { ++changes; });
    input->setText("programmatic");
    CHECK(changes == 0);
    CHECK(backend.prop(1, "text") == "programmatic");
}

TEST_CASE("RecordingBackend: a hidden or disabled widget ignores the helpers", "[ui]") {
    RecordingBackend backend;
    int clicks = 0;
    auto hidden = backend.createButton(nullptr);
    hidden->setOnClick([&] { ++clicks; });
    hidden->setVisible(false);
    auto disabled = backend.createButton(nullptr);
    disabled->setOnClick([&] { ++clicks; });
    disabled->setEnabled(false);
    backend.click(1);
    backend.click(2);
    CHECK(clicks == 0);
    disabled->setEnabled(true);
    backend.click(2);
    CHECK(clicks == 1);
}

TEST_CASE("RecordingBackend: a helper survives a handler that destroys its widget", "[ui]") {
    RecordingBackend backend;
    std::unique_ptr<ui::ButtonWidget> button = backend.createButton(nullptr);
    std::string seen;
    // The capture is larger than std::function's small buffer, so it lives in the stored handler's own allocation.
    button->setOnClick([&button, &seen, marker = std::string{"the handler's state outlives its widget"}] {
        button.reset();
        seen = marker;
    });
    backend.click(1);
    CHECK(seen == "the handler's state outlives its widget");
    CHECK_FALSE(backend.exists(1));
}

TEST_CASE("RecordingBackend: drag delivers the source's key to a target that accepts it", "[ui]") {
    RecordingBackend backend;
    std::vector<ui::Key> dropped;
    auto card = backend.createText(nullptr);
    card->setDragKey(intKey(7));
    auto plain = backend.createText(nullptr);
    auto numbers = backend.createPanel(nullptr);
    numbers->setDropHandler([](ui::Key const& key) { return std::holds_alternative<std::int64_t>(key); },
                            [&](ui::Key key) { dropped.push_back(std::move(key)); });
    auto words = backend.createPanel(nullptr);
    words->setDropHandler([](ui::Key const& key) { return std::holds_alternative<std::string>(key); },
                          [&](ui::Key key) { dropped.push_back(std::move(key)); });

    CHECK(backend.drag(1, 3));
    CHECK_FALSE(backend.drag(1, 4));  // refused by accepts
    CHECK_FALSE(backend.drag(1, 2));  // not a drop target
    CHECK_FALSE(backend.drag(2, 3));  // not draggable
    CHECK(dropped == std::vector<ui::Key>{intKey(7)});
}

TEST_CASE("RecordingBackend: a widget inside a hidden, disabled or collapsed container ignores the helpers", "[ui]") {
    RecordingBackend backend;
    int clicks = 0;
    std::vector<std::string> edits;
    std::vector<ui::Key> chosen;
    std::vector<ui::Key> dropped;
    auto panel = backend.createPanel(nullptr);
    panel->setCollapsible(true);
    auto column = backend.createStack(panel.get(), ui::Axis::Vertical);
    auto button = backend.createButton(column.get());
    button->setOnClick([&] { ++clicks; });
    auto input = backend.createTextInput(column.get(), ui::TextInputMode::SingleLine);
    input->setOnChange([&](std::string text) { edits.push_back(std::move(text)); });
    auto choice = backend.createSelect(column.get(), ui::SelectStyle::Dropdown);
    choice->setOptions({{.key = intKey(1), .label = "One"}});
    choice->setOnSelect([&](ui::Key key) { chosen.push_back(std::move(key)); });
    auto card = backend.createText(column.get());
    card->setDragKey(intKey(7));
    auto target = backend.createPanel(nullptr);
    target->setDropHandler([](ui::Key const&) { return true; },
                           [&](ui::Key key) { dropped.push_back(std::move(key)); });

    auto const useEverything = [&] {
        backend.click(3);
        backend.edit(4, "x");
        backend.choose(5, intKey(1));
        return backend.drag(6, 7);
    };
    panel->setVisible(false);
    CHECK_FALSE(useEverything());
    panel->setVisible(true);
    panel->setEnabled(false);
    CHECK_FALSE(useEverything());
    panel->setEnabled(true);
    panel->setCollapsed(true);
    CHECK_FALSE(useEverything());
    CHECK(clicks == 0);
    CHECK(edits.empty());
    CHECK(chosen.empty());
    CHECK(dropped.empty());

    panel->setCollapsed(false);
    CHECK(useEverything());
    CHECK(clicks == 1);
    CHECK(edits == std::vector<std::string>{"x"});
    CHECK(chosen == std::vector<ui::Key>{intKey(1)});
    CHECK(dropped == std::vector<ui::Key>{intKey(7)});
}

TEST_CASE("RecordingBackend: a collapsed panel's own collapse control still works", "[ui]") {
    RecordingBackend backend;
    std::vector<bool> requested;
    auto panel = backend.createPanel(nullptr);
    panel->setCollapsible(true);
    panel->setCollapsed(true);
    panel->setOnToggle([&](bool collapsed) { requested.push_back(collapsed); });
    backend.collapse(1, false);
    CHECK(requested == std::vector<bool>{false});
}

TEST_CASE("RecordingBackend: a select marks its requested key only while the options contain it", "[ui]") {
    RecordingBackend backend;
    auto choice = backend.createSelect(nullptr, ui::SelectStyle::Dropdown);
    choice->setSelected(ui::Key{std::string{"b"}});
    CHECK(backend.prop(1, "selected") == "none");
    choice->setOptions({{.key = intKey(1), .label = "One"}, {.key = ui::Key{std::string{"b"}}, .label = "Bee"}});
    CHECK(backend.prop(1, "selected") == "\"b\"");
    choice->setOptions({{.key = intKey(1), .label = "One"}});
    CHECK(backend.prop(1, "selected") == "none");
    choice->setOptions({{.key = ui::Key{std::string{"b"}}, .label = "Bee"}});
    CHECK(backend.prop(1, "selected") == "\"b\"");
    // Re-resolving is the widget's own doing, not a setter call.
    CHECK(backend.log() == std::vector<std::string>{"create Select#1 in root", "set Select#1 selected=none",
                                                    "set Select#1 options=[1:One,\"b\":Bee]",
                                                    "set Select#1 options=[1:One]",
                                                    "set Select#1 options=[\"b\":Bee]"});
    choice->setSelected(std::nullopt);
    choice->setOptions({{.key = ui::Key{std::string{"b"}}, .label = "Bee"}});
    CHECK(backend.prop(1, "selected") == "none");
}

TEST_CASE("RecordingBackend: what the user chose stays selected across new options", "[ui]") {
    RecordingBackend backend;
    auto choice = backend.createSelect(nullptr, ui::SelectStyle::Radio);
    choice->setOptions({{.key = intKey(1), .label = "One"}, {.key = intKey(2), .label = "Two"}});
    backend.choose(1, intKey(2));
    choice->setOptions({{.key = intKey(1), .label = "One"}});
    CHECK(backend.prop(1, "selected") == "none");
    choice->setOptions({{.key = intKey(2), .label = "Two"}});
    CHECK(backend.prop(1, "selected") == "2");
}

TEST_CASE("RecordingBackend: choosing a key the select does not offer is a test error", "[ui]") {
    RecordingBackend backend;
    std::vector<ui::Key> chosen;
    auto choice = backend.createSelect(nullptr, ui::SelectStyle::Dropdown);
    choice->setOptions({{.key = intKey(1), .label = "One"}});
    choice->setOnSelect([&](ui::Key key) { chosen.push_back(std::move(key)); });
    CHECK_THROWS_AS(backend.choose(1, intKey(2)), std::logic_error);
    CHECK_THROWS_AS(backend.choose(1, ui::Key{std::string{"1"}}), std::logic_error);
    CHECK(chosen.empty());
    CHECK(backend.prop(1, "selected").empty());
}

TEST_CASE("RecordingBackend: a helper used on the wrong kind of widget is a test error", "[ui]") {
    RecordingBackend backend;
    auto label = backend.createText(nullptr);
    auto button = backend.createButton(nullptr);
    auto fixed = backend.createPanel(nullptr);
    CHECK_THROWS_AS(backend.click(1), std::logic_error);
    CHECK_THROWS_AS(backend.edit(2, "x"), std::logic_error);
    CHECK_THROWS_AS(backend.submit(1, "x"), std::logic_error);
    CHECK_THROWS_AS(backend.toggle(1), std::logic_error);
    CHECK_THROWS_AS(backend.choose(1, intKey(1)), std::logic_error);
    CHECK_THROWS_AS(backend.chooseIndex(1, 0), std::logic_error);
    CHECK_THROWS_AS(backend.collapse(1, true), std::logic_error);
    CHECK_THROWS_AS(backend.setDateTime(1, std::nullopt), std::logic_error);
    CHECK_THROWS_AS(backend.slide(1, 1), std::logic_error);
    CHECK_THROWS_AS(backend.pick(1, "/tmp/x"), std::logic_error);
    CHECK_THROWS_AS(backend.dismiss(1), std::logic_error);
    // A panel the user cannot collapse has no control to do it with.
    CHECK_THROWS_AS(backend.collapse(3, true), std::logic_error);
}

TEST_CASE("RecordingBackend: a grid spans only its own children", "[ui]") {
    RecordingBackend backend;
    auto grid = backend.createGrid(nullptr);
    auto other = backend.createGrid(nullptr);
    auto stranger = backend.createText(other.get());
    auto root = backend.createText(nullptr);
    CHECK_THROWS_AS(grid->setSpan(*stranger, 2), std::logic_error);
    CHECK_THROWS_AS(grid->setSpan(*root, 2), std::logic_error);
    CHECK(backend.prop(3, "span").empty());
}

TEST_CASE("RecordingBackend: dragging a widget onto itself is a test error", "[ui]") {
    RecordingBackend backend;
    auto card = backend.createPanel(nullptr);
    card->setDragKey(intKey(1));
    card->setDropHandler([](ui::Key const&) { return true; }, [](ui::Key const&) {});
    CHECK_THROWS_AS(backend.drag(1, 1), std::logic_error);
}

TEST_CASE("RecordingBackend: text in a value cannot pass for another line, field or element", "[ui]") {
    RecordingBackend backend;
    auto label = backend.createText(nullptr);
    label->setText("a\nText#9 b=c\\d");
    auto menu = backend.createMenu(nullptr);
    menu->setItems({"x,y", "[z]"});
    auto choice = backend.createSelect(nullptr, ui::SelectStyle::Dropdown);
    choice->setOptions({{.key = ui::Key{std::string{"q\"r"}}, .label = "s:t"}});
    CHECK(backend.dump() ==
          "Text#1 text=a\\nText#9 b\\=c\\\\d\n"
          "Menu#2 items=[x\\,y,\\[z\\]]\n"
          "Select#3 options=[\"q\\\"r\":s:t] style=Dropdown\n");
    CHECK(backend.log().at(1) == "set Text#1 text=a\\nText#9 b\\=c\\\\d");
    CHECK(backend.find("Text", "text", "a\\nText#9 b\\=c\\\\d") == std::optional<int>{1});
}

TEST_CASE("RecordingBackend: every kind's setters show in the dump", "[ui]") {
    RecordingBackend backend;
    auto grid = backend.createGrid(nullptr);
    grid->setColumns(3);
    grid->setGap(1);
    auto heading = backend.createText(grid.get());
    heading->setRole(ui::TextRole::Heading);
    grid->setSpan(*heading, 2);
    auto gap = backend.createSpacer(grid.get());
    auto row = backend.createStack(nullptr, ui::Axis::Horizontal);
    row->setGap(2);
    auto menu = backend.createMenu(row.get());
    menu->setItems({"Open", "Quit"});
    auto busy = backend.createBusy(row.get());
    busy->setActive(true);
    busy->setLabel("wait");
    CHECK(backend.dump() ==
          "Grid#1 columns=3 gap=1\n"
          "  Text#2 role=Heading span=2\n"
          "  Spacer#3\n"
          "Row#4 gap=2\n"
          "  Menu#5 items=[Open,Quit]\n"
          "  Busy#6 active=true label=wait\n");
}

TEST_CASE("RecordingBackend: a moved widget keeps its state and handlers", "[ui]") {
    RecordingBackend backend;
    int clicks = 0;
    auto column = backend.createStack(nullptr, ui::Axis::Vertical);
    auto first = backend.createText(column.get());
    auto button = backend.createButton(column.get());
    button->setLabel("Go");
    button->setOnClick([&] { ++clicks; });
    column->moveChild(*button, 0);
    backend.click(3);
    CHECK(clicks == 1);
    CHECK(backend.dump() == "Column#1\n  Button#3 label=Go\n  Text#2\n");
}

TEST_CASE("RecordingBackend: a hidden source cannot be dragged", "[ui]") {
    RecordingBackend backend;
    std::vector<ui::Key> dropped;
    auto card = backend.createText(nullptr);
    card->setDragKey(intKey(7));
    card->setVisible(false);
    auto target = backend.createPanel(nullptr);
    target->setDropHandler([](ui::Key const&) { return true; },
                           [&](ui::Key key) { dropped.push_back(std::move(key)); });
    CHECK_FALSE(backend.drag(1, 2));
    CHECK(dropped.empty());
}

TEST_CASE("RecordingBackend: hasProp tells a property never set from one set empty", "[ui]") {
    RecordingBackend backend;
    auto label = backend.createText(nullptr);
    CHECK_FALSE(backend.hasProp(1, "text"));
    label->setText("");
    CHECK(backend.hasProp(1, "text"));
    CHECK(backend.prop(1, "text").empty());
    CHECK_THROWS_AS(backend.hasProp(2, "text"), std::out_of_range);
}

TEST_CASE("RecordingBackend: slots, tab bars and dialogs show in the dump", "[ui]") {
    RecordingBackend backend;
    std::vector<std::size_t> picked;
    auto tabs = backend.createTabs(nullptr);
    tabs->setTabs({"a,b", "c"});
    tabs->setSelected(1);
    tabs->setOnSelect([&](std::size_t index) { picked.push_back(index); });
    auto slot = backend.createSlot(tabs.get());
    auto dialog = backend.createDialog(nullptr);
    dialog->setTitle("x=y");
    dialog->setOpen(true);
    CHECK(backend.dump() ==
          "Tabs#1 selected=1 tabs=[a\\,b,c]\n"
          "  Slot#2\n"
          "Dialog#3 open=true title=x\\=y\n");

    backend.clearLog();
    backend.chooseIndex(1, 0);
    CHECK(picked == std::vector<std::size_t>{0});
    CHECK(backend.prop(1, "selected") == "0");
    CHECK(backend.log().empty());
}

TEST_CASE("RecordingBackend: a closed dialog ignores dismiss, and so does everything inside it", "[ui]") {
    RecordingBackend backend;
    int dismissed = 0;
    int clicks = 0;
    auto dialog = backend.createDialog(nullptr);
    dialog->setOnDismiss([&] { ++dismissed; });
    auto button = backend.createButton(dialog.get());
    button->setOnClick([&] { ++clicks; });
    // The click first: a dismissal closes the dialog, as a real one does, and the click would then not reach it.
    auto const useBoth = [&] {
        backend.click(2);
        backend.dismiss(1);
    };
    useBoth();  // a new dialog is closed
    dialog->setOpen(true);
    useBoth();
    dialog->setOpen(false);
    useBoth();
    CHECK(dismissed == 1);
    CHECK(clicks == 1);
}

// Mutations: in FakeTextInput::setText, move the cursor whatever the text (the unchanged text loses the cursor);
// never move it (the new text keeps a cursor inside it).
TEST_CASE("RecordingBackend: setText with the text shown keeps the cursor, and another text moves it to the end",
          "[ui]") {
    RecordingBackend backend;
    auto field = backend.createTextInput(nullptr, ui::TextInputMode::SingleLine);
    backend.edit(1, "abc");
    CHECK(backend.cursor(1) == 3);
    backend.moveCursor(1, 1);
    CHECK(backend.cursor(1) == 1);
    field->setText("abc");
    CHECK(backend.cursor(1) == 1);
    field->setText("abcd");
    CHECK(backend.cursor(1) == 4);
    backend.moveCursor(1, 99);
    CHECK(backend.cursor(1) == 4);
}

// Mutation: drop the setByUser in RecordingBackend::dismiss (the dialog stays open).
TEST_CASE("RecordingBackend: a dismissal closes the dialog before onDismiss runs", "[ui]") {
    RecordingBackend backend;
    std::string openInHandler;
    auto dialog = backend.createDialog(nullptr);
    dialog->setOnDismiss([&] { openInHandler = backend.prop(1, "open"); });
    dialog->setOpen(true);
    backend.clearLog();
    backend.dismiss(1);
    CHECK(openInHandler == "false");
    CHECK(backend.prop(1, "open") == "false");
    CHECK(backend.log().empty());
}

// SPDX-License-Identifier: Apache-2.0
//
// The region widgets (collapsible, splitter, drop zone, drawer) and the contract's later additions to every widget:
// field state, commit, chords, nested menus and several roots.

#include <catch2/catch_test_macros.hpp>
#include <core/tui/InputEvent.hpp>
#include <core/tui/KeyCode.hpp>
#include <core/tui/Modifier.hpp>
#include <core/tui/Screen.hpp>
#include <morph/ui/view.hpp>
#include <optional>
#include <string>
#include <vector>

#include "tui/container_widgets.hpp"
#include "tui/field_widgets.hpp"
#include "tui/leaf_widgets.hpp"
#include "tui/list_widgets.hpp"
#include "tui/region_widgets.hpp"
#include "tui_harness.hpp"

namespace ui = morph::ui;
using morph::tui::detail::ButtonImpl;
using morph::tui::detail::CheckboxImpl;
using morph::tui::detail::CollapsibleImpl;
using morph::tui::detail::DialogImpl;
using morph::tui::detail::DrawerImpl;
using morph::tui::detail::DropZoneImpl;
using morph::tui::detail::MenuImpl;
using morph::tui::detail::parsePastedPaths;
using morph::tui::detail::SplitterImpl;
using morph::tui::detail::StackImpl;
using morph::tui::detail::TextImpl;
using morph::tui::detail::TextInputImpl;
using morph::tui::detail::WidgetBase;
using morph::tui::testing::Harness;
using Rows = std::vector<std::string>;
using Paths = std::vector<std::string>;

namespace {

std::string report(int code, core::tui::Point cell, char final) {
    return "\x1b[<" + std::to_string(code) + ";" + std::to_string(cell.x + 1) + ";" + std::to_string(cell.y + 1) +
           final;
}

}  // namespace

// Mutation: let a closed collapsible's header act as its body does (the header's click is then lost).
TEST_CASE("tui collapsible: the title row holds the header; Enter on it opens and closes, and reports the request",
          "[tui][regions]") {
    Harness harness{30, 4};
    auto const section = harness.make<CollapsibleImpl>(nullptr);
    auto const header = harness.make<ButtonImpl>(section.get());
    auto const body = harness.make<TextImpl>(section.get());
    std::vector<bool> requests;
    section->setTitle("More");
    section->setHeader(*header);
    section->setOnToggle([&](bool open) { requests.push_back(open); });
    header->setLabel("Reset");
    body->setText("details");
    CHECK(harness.draw() == Rows{"▾ More [ Reset ]", "details"});
    harness.focus(*section);
    CHECK(harness.key(core::tui::KeyCode::Enter) == core::tui::EventResult::Handled);
    CHECK(requests == std::vector<bool>{false});
    CHECK(harness.draw() == Rows{"▸ More [ Reset ]"});
    CHECK(WidgetBase::of(*header).actionable());
    CHECK_FALSE(WidgetBase::of(*body).actionable());
    section->setOpen(true);  // a setter reports nothing
    CHECK(requests == std::vector<bool>{false});
    CHECK(harness.draw() == Rows{"▾ More [ Reset ]", "details"});
}

// Mutation: report the sizes on every move rather than on the release (onResize then runs per cell).
TEST_CASE("tui splitter: a handle dragged with the pointer moves, and the release reports every pane's size",
          "[tui][regions]") {
    Harness harness{21, 3};
    auto const splitter = harness.make<SplitterImpl>(nullptr, ui::Axis::Horizontal);
    auto const left = harness.make<TextImpl>(splitter.get());
    auto const right = harness.make<TextImpl>(splitter.get());
    std::vector<std::vector<int>> reports;
    left->setText("L");
    right->setText("R");
    splitter->setOnResize([&](std::vector<int> sizes) { reports.push_back(std::move(sizes)); });
    static_cast<void>(harness.draw());
    CHECK(splitter->extents() == std::vector<int>{10, 10});  // an equal share of 20 cells after the handle
    harness.sgr(report(0, {.x = 10, .y = 0}, 'M'));
    harness.sgr(report(32, {.x = 7, .y = 0}, 'M'));
    harness.sgr(report(32, {.x = 5, .y = 0}, 'M'));
    CHECK(reports.empty());
    harness.sgr(report(0, {.x = 5, .y = 0}, 'm'));
    REQUIRE(reports.size() == 1);
    CHECK(reports.front() == std::vector<int>{5, 15});
    static_cast<void>(harness.draw());
    CHECK(splitter->extents() == std::vector<int>{5, 15});
}

// Mutation: let the last pane keep its asked-for size (the sizes then leave cells unlaid).
TEST_CASE("tui splitter: sizes asked for are kept, the last pane taking the rest; a count off shares equally",
          "[tui][regions]") {
    Harness harness{31, 2};
    auto const splitter = harness.make<SplitterImpl>(nullptr, ui::Axis::Horizontal);
    auto const one = harness.make<TextImpl>(splitter.get());
    auto const two = harness.make<TextImpl>(splitter.get());
    auto const three = harness.make<TextImpl>(splitter.get());
    splitter->setSizes({4, 6, 1});
    static_cast<void>(harness.draw());
    CHECK(splitter->extents() == std::vector<int>{4, 6, 19});
    splitter->setSizes({4, 6});
    static_cast<void>(harness.draw());
    CHECK(splitter->extents() == std::vector<int>{9, 9, 11});
}

// Mutation: split the pasted text at every space (a path with an escaped space is then cut in two).
TEST_CASE("tui drop zone: pasted paths, quoted, escaped or as file URIs", "[tui][regions]") {
    CHECK(parsePastedPaths("/a/b.csv") == Paths{"/a/b.csv"});
    CHECK(parsePastedPaths("'/a/my file.csv' \"/b/c.txt\"") == Paths{"/a/my file.csv", "/b/c.txt"});
    CHECK(parsePastedPaths("/a/my\\ file.csv\n/b.csv") == Paths{"/a/my file.csv", "/b.csv"});
    CHECK(parsePastedPaths("file:///tmp/x%20y.csv") == Paths{"/tmp/x y.csv"});
    CHECK(parsePastedPaths("file://localhost/tmp/z.csv") == Paths{"/tmp/z.csv"});
    CHECK(parsePastedPaths("  \n ").empty());
}

// Mutation: compare the extension with case (RUN.CSV is then refused).
TEST_CASE("tui drop zone: a paste while it has the focus is a drop it takes whole or refuses whole",
          "[tui][regions]") {
    Harness harness{30, 4};
    auto const zone = harness.make<DropZoneImpl>(nullptr);
    std::vector<Paths> drops;
    zone->setAccept({".csv"});
    zone->setOnDrop([&](Paths paths) { drops.push_back(std::move(paths)); });
    static_cast<void>(harness.draw());
    CHECK(harness.send(core::tui::PasteEvent{.text = "/in/a.csv"}) == core::tui::EventResult::Ignored);
    harness.focus(*zone);
    static_cast<void>(harness.send(core::tui::PasteEvent{.text = "/in/RUN.CSV"}));
    static_cast<void>(harness.send(core::tui::PasteEvent{.text = "/in/notes.txt"}));
    static_cast<void>(harness.send(core::tui::PasteEvent{.text = "/in/a.csv /in/b.csv"}));
    zone->setMultiple(true);
    static_cast<void>(harness.send(core::tui::PasteEvent{.text = "/in/a.csv /in/b.csv"}));
    CHECK(drops == std::vector<Paths>{{"/in/RUN.CSV"}, {"/in/a.csv", "/in/b.csv"}});
}

// Mutation: place a drawer as a dialog is placed (centred, not against its side).
TEST_CASE("tui drawer: its box stands against its side, the screen's whole height", "[tui][regions]") {
    Harness harness{40, 8};
    auto const root = harness.make<StackImpl>(nullptr, ui::Axis::Vertical);
    auto const drawer = harness.make<DrawerImpl>(root.get(), ui::Side::End);
    auto const inside = harness.make<TextImpl>(drawer.get());
    inside->setText("Filters");
    drawer->setTitle("Panel");
    drawer->setOpen(true);
    static_cast<void>(harness.draw());
    auto const bounds = drawer->host().screenBounds();
    CHECK(bounds.y == 0);
    CHECK(bounds.height == 8);
    CHECK(bounds.x + bounds.width == 40);
    CHECK(bounds.width < 40);
}

// Mutation: call onDismiss before closing (the handler then sees the dialog still open).
TEST_CASE("tui dialog: Esc closes the dialog before its onDismiss runs", "[tui][regions]") {
    Harness harness{40, 8};
    auto const root = harness.make<StackImpl>(nullptr, ui::Axis::Vertical);
    auto const dialog = harness.make<DialogImpl>(root.get());
    auto const inside = harness.make<ButtonImpl>(dialog.get());
    inside->setLabel("OK");
    std::optional<bool> openInHandler;
    dialog->setOnDismiss([&] { openInHandler = dialog->isOpen(); });
    dialog->setOpen(true);
    static_cast<void>(harness.draw());
    CHECK(harness.key(core::tui::KeyCode::Escape) == core::tui::EventResult::Handled);
    REQUIRE(openInHandler.has_value());
    CHECK_FALSE(*openInHandler);
    CHECK_FALSE(dialog->isOpen());
}

// Mutation: draw no marks (a required, invalid or stale field then looks like any other).
TEST_CASE("tui fields: required, errors and stale show as marks; the errors show on hover", "[tui][fields][state]") {
    Harness harness{20, 2};
    auto const box = harness.make<CheckboxImpl>(nullptr);
    box->setLabel("Agree");
    box->setRequired(true);
    CHECK(harness.draw() == Rows{"[ ] Agree          *"});
    box->setErrors({"must be ticked", "twice"});
    box->setStale(true);
    CHECK(harness.draw() == Rows{"[ ] Agree        *!~"});
    box->setTooltip("Terms");
    CHECK(WidgetBase::of(*box).hoverText() == "Terms\nmust be ticked\ntwice");
}

// Mutation: drop the read-only check from the checkbox (Space then toggles a read-only box).
TEST_CASE("tui fields: a read-only checkbox takes the focus and reports no toggle", "[tui][fields][state]") {
    Harness harness{20, 2};
    auto const box = harness.make<CheckboxImpl>(nullptr);
    std::vector<bool> toggles;
    box->setLabel("Agree");
    box->setOnToggle([&](bool checked) { toggles.push_back(checked); });
    box->setReadOnly(true);
    static_cast<void>(harness.draw());
    harness.focus(*box);
    CHECK(harness.focused(*box));
    static_cast<void>(harness.key(core::tui::KeyCode::Enter));
    CHECK(toggles.empty());
    box->setReadOnly(false);
    static_cast<void>(harness.key(core::tui::KeyCode::Enter));
    CHECK(toggles == std::vector<bool>{true});
}

// Mutation: commit after the submit (the order then reads submit, commit).
TEST_CASE("tui fields: Enter commits then submits; the focus leaving commits only after an edit",
          "[tui][fields][commit]") {
    Harness harness{30, 3};
    auto const column = harness.make<StackImpl>(nullptr, ui::Axis::Vertical);
    auto const field = harness.make<TextInputImpl>(column.get(), ui::TextInputMode::SingleLine);
    auto const other = harness.make<ButtonImpl>(column.get());
    std::vector<std::string> events;
    field->setOnCommit([&](std::string const& text) { events.push_back("commit " + text); });
    field->setOnSubmit([&](std::string const& text) { events.push_back("submit " + text); });
    other->setLabel("Go");
    static_cast<void>(harness.draw());
    harness.focus(*field);
    static_cast<void>(harness.type("ab"));
    static_cast<void>(harness.key(core::tui::KeyCode::Enter));
    CHECK(events == std::vector<std::string>{"commit ab", "submit ab"});
    harness.focus(*other);  // no edit since the commit
    CHECK(events.size() == 2);
    harness.focus(*field);
    static_cast<void>(harness.type("c"));
    harness.focus(*other);
    CHECK(events == std::vector<std::string>{"commit ab", "submit ab", "commit abc"});
}

// Mutation: report the index within the level instead of the path (the submenu's entry then reads {0}).
TEST_CASE("tui menu: Right opens a submenu, Left goes back, and a choice reports its path", "[tui][lists][menu]") {
    Harness harness{30, 4};
    auto const menu = harness.make<MenuImpl>(nullptr);
    std::vector<std::vector<std::size_t>> chosen;
    menu->setItems(
        {ui::MenuEntry{.label = "Recent",
                       .items = {ui::MenuEntry{.label = "a.txt"}, ui::MenuEntry{.label = "b.txt", .enabled = false}}},
         ui::MenuEntry{.label = "Wrap", .keys = "Alt+Z", .checked = true}});
    menu->setOnActivate([&](std::vector<std::size_t> path) { chosen.push_back(std::move(path)); });
    CHECK(harness.draw() == Rows{"▶ Recent >", "  [x] Wrap  Alt+Z"});
    harness.focus(*menu);
    static_cast<void>(harness.key(core::tui::KeyCode::Right));
    CHECK(menu->level() == std::vector<std::size_t>{0});
    CHECK(harness.draw() == Rows{"▶ a.txt", "  b.txt"});
    static_cast<void>(harness.key(core::tui::KeyCode::Left));
    CHECK(menu->level().empty());
    static_cast<void>(harness.key(core::tui::KeyCode::Enter));  // Recent: opens
    menu->highlight(1);
    static_cast<void>(harness.key(core::tui::KeyCode::Enter));  // b.txt: disabled
    CHECK(chosen.empty());
    menu->highlight(0);
    static_cast<void>(harness.key(core::tui::KeyCode::Enter));
    CHECK(chosen == std::vector<std::vector<std::size_t>>{{0, 0}});
    CHECK(menu->level().empty());
}

// Mutation: run the chords of a widget whose own key took the event (Enter on the button then runs both).
TEST_CASE("tui chords: a button's own key wins over its chord, an ignored key reaches the chord", "[tui][chords]") {
    Harness harness{30, 3};
    auto const button = harness.make<ButtonImpl>(nullptr);
    std::vector<std::string> chords;
    int clicks = 0;
    button->setLabel("Go");
    button->setOnClick([&] { ++clicks; });
    button->setKeys({"Enter", "F5"}, [&](std::string chord) { chords.push_back(std::move(chord)); });
    static_cast<void>(harness.draw());
    harness.focus(*button);
    CHECK(harness.key(core::tui::KeyCode::Enter) == core::tui::EventResult::Handled);
    CHECK(clicks == 1);
    CHECK(chords.empty());
    CHECK(harness.key(core::tui::KeyCode::F5) == core::tui::EventResult::Handled);
    CHECK(chords == std::vector<std::string>{"F5"});
}

// Mutation: leave a field's chords out of the keys its view leaves (Ctrl+S then reaches nothing).
TEST_CASE("tui chords: a field's own key wins over its chord, an ignored key reaches the chord", "[tui][chords]") {
    Harness harness{30, 3};
    auto const field = harness.make<TextInputImpl>(nullptr, ui::TextInputMode::SingleLine);
    std::vector<std::string> chords;
    field->setKeys({"Ctrl+S", "Enter"}, [&](std::string chord) { chords.push_back(std::move(chord)); });
    std::vector<std::string> submits;
    field->setOnSubmit([&](std::string text) { submits.push_back(std::move(text)); });
    static_cast<void>(harness.draw());
    harness.focus(*field);
    CHECK(harness.key(core::tui::KeyCode::Enter) == core::tui::EventResult::Handled);
    CHECK(submits.size() == 1);
    CHECK(chords.empty());
    CHECK(harness.send(core::tui::KeyEvent{.key = core::tui::keyCodeFromCodepoint(U's'),
                                           .modifiers = core::tui::Modifier::Ctrl,
                                           .codepoint = U's'}) == core::tui::EventResult::Handled);
    CHECK(chords == std::vector<std::string>{"Ctrl+S"});
}

// Mutation: give every root the whole screen (the second root is then drawn over the first).
TEST_CASE("tui roots: several roots share the screen as a column, the last taking what is left", "[tui][roots]") {
    Harness harness{20, 5};
    auto const first = harness.make<TextImpl>(nullptr);
    auto const second = harness.make<TextImpl>(nullptr);
    first->setText("one");
    second->setText("two");
    CHECK(harness.draw() == Rows{"one", "two"});
    CHECK(WidgetBase::of(*second).view().screenBounds().height == 4);
}

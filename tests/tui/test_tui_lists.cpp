// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <core/tui/Canvas.hpp>
#include <core/tui/InputEvent.hpp>
#include <core/tui/KeyCode.hpp>
#include <core/tui/Rect.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <morph/ui/backend.hpp>
#include <morph/ui/view.hpp>
#include <optional>
#include <string>
#include <vector>

#include "tui/container_widgets.hpp"
#include "tui/field_widgets.hpp"
#include "tui/leaf_widgets.hpp"
#include "tui/list_widgets.hpp"
#include "tui_harness.hpp"

namespace ui = morph::ui;
using core::tui::EventResult;
using core::tui::KeyCode;
using morph::tui::detail::ButtonImpl;
using morph::tui::detail::Context;
using morph::tui::detail::Direction;
using morph::tui::detail::DropdownSelectImpl;
using morph::tui::detail::FilePickerImpl;
using morph::tui::detail::MenuImpl;
using morph::tui::detail::PanelImpl;
using morph::tui::detail::RadioSelectImpl;
using morph::tui::detail::SpacerImpl;
using morph::tui::detail::StackImpl;
using morph::tui::detail::TabsImpl;
using morph::tui::detail::TextImpl;
using morph::tui::detail::WidgetBase;
using morph::tui::testing::Harness;
using Rows = std::vector<std::string>;

namespace {

ui::Key key(std::int64_t value) { return ui::Key{value}; }

std::vector<ui::SelectOption> colours() {
    return {{.key = key(1), .label = "Red"}, {.key = key(2), .label = "Green"}};
}

/// Whether any row of @p rows contains @p text.
bool shows(Rows const& rows, std::string const& text) {
    return std::ranges::any_of(rows, [&text](std::string const& row) { return row.contains(text); });
}

/// A container that holds its children out of reach without telling anyone, as a container may whose own state
/// decides that (a dialog closing).
class Gate final : public morph::tui::detail::TuiContainer<ui::SlotWidget> {
public:
    explicit Gate(Context& context) : TuiContainer{context} { adopt(morph::tui::detail::makeView(*this)); }
    void setOpen(bool open) noexcept { _open = open; }
    [[nodiscard]] core::tui::Size naturalSize() const override {
        return morph::tui::detail::stackNaturalSize(children(), ui::Axis::Vertical, 0);
    }
    [[nodiscard]] bool letsChildrenAct() const override { return _open; }
    void paint(core::tui::Canvas& canvas) override {
        morph::tui::detail::arrangeStack(children(),
                                         {.x = 0, .y = 0, .width = canvas.width(), .height = canvas.height()}, {});
    }

private:
    bool _open = true;
};

}  // namespace

TEST_CASE("tui lists: a radio select marks the selection and selects on Space", "[tui][lists]") {
    Harness harness{20, 2};
    auto const select = harness.make<RadioSelectImpl>(nullptr);
    std::vector<ui::Key> chosen;
    select->setOptions(colours());
    select->setSelected(key(2));
    select->setOnSelect([&](ui::Key picked) { chosen.push_back(std::move(picked)); });
    CHECK(harness.draw() == Rows{"  ( ) Red", "▶ (•) Green"});
    harness.focus(*select);
    static_cast<void>(harness.key(KeyCode::Up));
    static_cast<void>(harness.type(" "));
    CHECK(chosen == std::vector<ui::Key>{key(1)});
    CHECK(harness.draw() == Rows{"▶ (•) Red", "  ( ) Green"});
}

TEST_CASE("tui lists: a dropdown opens its list as an overlay and closes on a choice", "[tui][lists]") {
    Harness harness{20, 4};
    auto const select = harness.make<DropdownSelectImpl>(nullptr);
    std::vector<ui::Key> chosen;
    select->setOptions(colours());
    select->setSelected(key(1));
    select->setOnSelect([&](ui::Key picked) { chosen.push_back(std::move(picked)); });
    CHECK(harness.draw() == Rows{"[Red ▾]"});
    harness.focus(*select);
    static_cast<void>(harness.key(KeyCode::Enter));
    CHECK(select->isOpen());
    CHECK(harness.draw() == Rows{"[Red ▾]", "▶ Red", "  Green"});
    static_cast<void>(harness.key(KeyCode::Down));
    static_cast<void>(harness.key(KeyCode::Enter));
    CHECK_FALSE(select->isOpen());
    CHECK(chosen == std::vector<ui::Key>{key(2)});
    CHECK(harness.focused(*select));
    CHECK(harness.draw() == Rows{"[Green ▾]"});
}

TEST_CASE("tui lists: Esc closes a dropdown without choosing", "[tui][lists]") {
    Harness harness{20, 4};
    auto const select = harness.make<DropdownSelectImpl>(nullptr);
    int chosen = 0;
    select->setOptions(colours());
    select->setOnSelect([&](ui::Key const&) { ++chosen; });
    harness.focus(*select);
    static_cast<void>(harness.key(KeyCode::Enter));
    static_cast<void>(harness.key(KeyCode::Escape));
    CHECK_FALSE(select->isOpen());
    CHECK(chosen == 0);
    CHECK(harness.focused(*select));
}

TEST_CASE("tui lists: a menu is one list; Enter activates the highlighted item", "[tui][lists]") {
    Harness harness{20, 2};
    auto const menu = harness.make<MenuImpl>(nullptr);
    std::vector<std::size_t> activated;
    menu->setItems({"Open", "Quit"});
    menu->setOnActivate([&](std::size_t index) { activated.push_back(index); });
    CHECK(harness.draw() == Rows{"▶ Open", "  Quit"});
    harness.focus(*menu);
    static_cast<void>(harness.key(KeyCode::Down));
    static_cast<void>(harness.key(KeyCode::Enter));
    CHECK(activated == std::vector<std::size_t>{1});
}

TEST_CASE("tui lists: tabs draw a bar over the pages the mount leaves visible; Right selects the next",
          "[tui][lists]") {
    Harness harness{20, 3};
    auto const tabs = harness.make<TabsImpl>(nullptr);
    std::vector<std::size_t> selected;
    tabs->setTabs({"One", "Two"});
    tabs->setSelected(0);
    tabs->setOnSelect([&](std::size_t index) { selected.push_back(index); });
    // As the mount does it: one page per tab shown so far, in first-shown order, every page but the selected one
    // hidden through its own visible flag.
    auto const first = harness.make<TextImpl>(tabs.get());
    first->setText("first");
    auto const second = harness.make<TextImpl>(tabs.get());
    second->setText("second");
    second->setVisible(false);
    CHECK(harness.draw() == Rows{"[One] Two", "first"});
    harness.focus(*tabs);
    static_cast<void>(harness.key(KeyCode::Right));
    CHECK(selected == std::vector<std::size_t>{1});
    // The bar moves at once; the pages stay as they are until the mount swaps them.
    CHECK(harness.draw() == Rows{" One [Two]", "first"});
    first->setVisible(false);
    second->setVisible(true);
    CHECK(harness.draw() == Rows{" One [Two]", "second"});
    static_cast<void>(harness.key(KeyCode::Right));
    CHECK(selected.size() == 1);
}

TEST_CASE("tui lists: a radio select and a menu leave Tab and Esc to the frontend and dialogs", "[tui][lists]") {
    Harness harness{20, 4};
    auto const column = harness.make<StackImpl>(nullptr, ui::Axis::Vertical);
    auto const radio = harness.make<RadioSelectImpl>(column.get());
    auto const menu = harness.make<MenuImpl>(column.get());
    radio->setOptions(colours());
    menu->setItems({"Open", "Quit"});
    for (ui::Widget const* list :
         {static_cast<ui::Widget const*>(radio.get()), static_cast<ui::Widget const*>(menu.get())}) {
        harness.focus(*list);
        CHECK(harness.key(KeyCode::Escape) == EventResult::Ignored);
        CHECK(harness.key(KeyCode::Tab) == EventResult::Ignored);
        CHECK(harness.focused(*list));
    }
}

TEST_CASE("tui lists: a tab's page is not picked by its position among the children", "[tui][lists]") {
    Harness harness{20, 3};
    auto const tabs = harness.make<TabsImpl>(nullptr);
    tabs->setTabs({"One", "Two"});
    // Tab Two was shown first, so its page is the first child; tab One's page arrived second and is selected.
    auto const twoPage = harness.make<TextImpl>(tabs.get());
    twoPage->setText("two");
    twoPage->setVisible(false);
    auto const onePage = harness.make<TextImpl>(tabs.get());
    onePage->setText("one");
    tabs->setSelected(0);
    CHECK(harness.draw() == Rows{"[One] Two", "one"});
}

TEST_CASE("tui lists: a select keeps a requested key across options that lack it, and no setter calls a handler",
          "[tui][lists][selection]") {
    Harness harness{20, 4};
    auto const column = harness.make<StackImpl>(nullptr, ui::Axis::Vertical);
    auto const radio = harness.make<RadioSelectImpl>(column.get());
    auto const dropdown = harness.make<DropdownSelectImpl>(column.get());
    int calls = 0;
    radio->setOnSelect([&](ui::Key const&) { ++calls; });
    dropdown->setOnSelect([&](ui::Key const&) { ++calls; });
    std::vector<ui::SelectOption> const withBlue{{.key = key(1), .label = "Red"}, {.key = key(3), .label = "Blue"}};
    for (ui::SelectWidget* const select :
         {static_cast<ui::SelectWidget*>(radio.get()), static_cast<ui::SelectWidget*>(dropdown.get())}) {
        // The key arrives before options that contain it.
        select->setSelected(key(3));
        select->setOptions(colours());
    }
    CHECK(radio->probeText().empty());
    CHECK(dropdown->probeText().empty());
    CHECK(harness.draw() == Rows{"▶ ( ) Red", "  ( ) Green", "[ ▾]"});

    radio->setOptions(withBlue);
    dropdown->setOptions(withBlue);
    CHECK(radio->probeText() == "Blue");
    CHECK(dropdown->probeText() == "Blue");
    CHECK(harness.draw() == Rows{"▶ ( ) Red", "  (•) Blue", "[Blue ▾]"});

    radio->setOptions(colours());
    dropdown->setOptions(colours());
    CHECK(radio->probeText().empty());
    CHECK(dropdown->probeText().empty());
    radio->setOptions(withBlue);
    dropdown->setOptions(withBlue);
    CHECK(radio->probeText() == "Blue");
    CHECK(dropdown->probeText() == "Blue");

    radio->setSelected(std::nullopt);
    dropdown->setSelected(std::nullopt);
    CHECK(radio->probeText().empty());
    CHECK(dropdown->probeText().empty());
    CHECK(calls == 0);
}

TEST_CASE("tui lists: no setter calls a handler, and an open list follows new options", "[tui][lists][selection]") {
    Harness harness{20, 6};
    auto const column = harness.make<StackImpl>(nullptr, ui::Axis::Vertical);
    auto const dropdown = harness.make<DropdownSelectImpl>(column.get());
    auto const menu = harness.make<MenuImpl>(column.get());
    auto const tabs = harness.make<TabsImpl>(column.get());
    int calls = 0;
    std::vector<ui::Key> chosen;
    dropdown->setOnSelect([&](ui::Key picked) { chosen.push_back(std::move(picked)); });
    menu->setOnActivate([&](std::size_t /*index*/) { ++calls; });
    tabs->setOnSelect([&](std::size_t /*index*/) { ++calls; });
    dropdown->setOptions(colours());
    dropdown->setSelected(key(2));
    harness.focus(*dropdown);
    static_cast<void>(harness.key(KeyCode::Enter));
    REQUIRE(dropdown->isOpen());

    dropdown->setOptions({{.key = key(2), .label = "Green"}, {.key = key(3), .label = "Blue"}});
    dropdown->setSelected(key(3));
    menu->setItems({"A"});
    menu->setItems({"A", "B"});
    tabs->setTabs({"One", "Two"});
    tabs->setSelected(1);
    tabs->setSelected(5);
    CHECK(calls == 0);
    CHECK(chosen.empty());
    CHECK(dropdown->isOpen());
    CHECK(harness.draw() == Rows{"[Blue ▾]", "▶ Green", "  Blue", " One  Two"});

    static_cast<void>(harness.key(KeyCode::Down));
    static_cast<void>(harness.key(KeyCode::Enter));
    CHECK(chosen == std::vector<ui::Key>{key(3)});
}

TEST_CASE("tui lists: a radio select at its natural width shows whole labels", "[tui][lists]") {
    Harness harness{20, 2};
    auto const row = harness.make<StackImpl>(nullptr, ui::Axis::Horizontal);
    auto const radio = harness.make<RadioSelectImpl>(row.get());
    auto const after = harness.make<TextImpl>(row.get());
    after->setText("|");
    radio->setOptions(colours());
    radio->setSelected(key(2));
    CHECK(harness.draw() == Rows{"  ( ) Red  |", "▶ (•) Green"});
}

TEST_CASE("tui lists: a list with fewer rows than items scrolls to keep the highlight in view", "[tui][lists]") {
    Harness harness{20, 4};
    auto const column = harness.make<StackImpl>(nullptr, ui::Axis::Vertical);
    auto const menu = harness.make<MenuImpl>(column.get());
    menu->setLayout({.width = ui::Sizing::content(), .height = ui::Sizing::fixed(2)});
    auto const after = harness.make<TextImpl>(column.get());
    after->setText("end");
    std::vector<std::size_t> activated;
    menu->setOnActivate([&](std::size_t index) { activated.push_back(index); });
    menu->setItems({"A", "B", "C", "D"});
    CHECK(harness.draw() == Rows{"▶ A", "  B", "end"});
    harness.focus(*menu);
    static_cast<void>(harness.key(KeyCode::Down));
    static_cast<void>(harness.key(KeyCode::Down));
    CHECK(harness.draw() == Rows{"  B", "▶ C", "end"});
    static_cast<void>(harness.key(KeyCode::Up));
    CHECK(harness.draw() == Rows{"▶ B", "  C", "end"});
    static_cast<void>(harness.key(KeyCode::Up));
    CHECK(harness.draw() == Rows{"▶ A", "  B", "end"});
    static_cast<void>(harness.key(KeyCode::End));
    CHECK(harness.draw() == Rows{"  C", "▶ D", "end"});
    // A click lands on the row shown there, not on the row at that position from the top.
    CHECK(harness.click({.x = 1, .y = 0}) == EventResult::Handled);
    CHECK(activated == std::vector<std::size_t>{2});
}

TEST_CASE("tui lists: a click selects a radio option, activates a menu item and picks a tab", "[tui][lists][click]") {
    Harness harness{20, 6};
    auto const column = harness.make<StackImpl>(nullptr, ui::Axis::Vertical);
    auto const radio = harness.make<RadioSelectImpl>(column.get());
    auto const menu = harness.make<MenuImpl>(column.get());
    auto const tabs = harness.make<TabsImpl>(column.get());
    std::vector<ui::Key> chosen;
    std::vector<std::size_t> activated;
    std::vector<std::size_t> selected;
    radio->setOptions(colours());
    radio->setOnSelect([&](ui::Key picked) { chosen.push_back(std::move(picked)); });
    menu->setItems({"Open", "Quit"});
    menu->setOnActivate([&](std::size_t index) { activated.push_back(index); });
    tabs->setTabs({"One", "Two"});
    tabs->setSelected(0);
    tabs->setOnSelect([&](std::size_t index) { selected.push_back(index); });
    CHECK(harness.draw() == Rows{"▶ ( ) Red", "  ( ) Green", "▶ Open", "  Quit", "[One] Two"});

    CHECK(harness.click({.x = 3, .y = 1}) == EventResult::Handled);
    CHECK(chosen == std::vector<ui::Key>{key(2)});
    CHECK(harness.focused(*radio));
    CHECK(harness.click({.x = 3, .y = 3}) == EventResult::Handled);
    CHECK(activated == std::vector<std::size_t>{1});
    CHECK(harness.focused(*menu));
    CHECK(harness.click({.x = 7, .y = 4}) == EventResult::Handled);
    CHECK(selected == std::vector<std::size_t>{1});
    CHECK(harness.focused(*tabs));
    CHECK(harness.draw() == Rows{"  ( ) Red", "▶ (•) Green", "  Open", "▶ Quit", " One [Two]"});
}

TEST_CASE("tui lists: a click opens a dropdown, and a click on an option picks it", "[tui][lists][click]") {
    Harness harness{20, 4};
    auto const column = harness.make<StackImpl>(nullptr, ui::Axis::Vertical);
    auto const select = harness.make<DropdownSelectImpl>(column.get());
    auto const below = harness.make<ButtonImpl>(column.get());
    below->setLabel("Go");
    int clicks = 0;
    below->setOnClick([&] { ++clicks; });
    std::vector<ui::Key> chosen;
    select->setOptions(colours());
    select->setSelected(key(1));
    select->setOnSelect([&](ui::Key picked) { chosen.push_back(std::move(picked)); });
    CHECK(harness.draw() == Rows{"[Red ▾]", "[ Go ]"});
    CHECK(harness.click({.x = 1, .y = 0}) == EventResult::Handled);
    CHECK(select->isOpen());
    // The list covers what lies below the field.
    CHECK(harness.draw() == Rows{"[Red ▾]", "▶ Red", "  Green"});
    CHECK(harness.click({.x = 3, .y = 2}) == EventResult::Handled);
    CHECK_FALSE(select->isOpen());
    CHECK(chosen == std::vector<ui::Key>{key(2)});
    CHECK(clicks == 0);
    CHECK(harness.focused(*select));
    CHECK(harness.draw() == Rows{"[Green ▾]", "[ Go ]"});
}

TEST_CASE("tui lists: a dropdown at the bottom opens its list above itself", "[tui][lists]") {
    Harness harness{20, 4};
    auto const column = harness.make<StackImpl>(nullptr, ui::Axis::Vertical);
    auto const spacer = harness.make<SpacerImpl>(column.get());
    auto const select = harness.make<DropdownSelectImpl>(column.get());
    select->setOptions(colours());
    select->setSelected(key(1));
    CHECK(harness.draw() == Rows{"", "", "", "[Red ▾]"});
    harness.focus(*select);
    static_cast<void>(harness.key(KeyCode::Enter));
    CHECK(harness.draw() == Rows{"", "▶ Red", "  Green", "[Red ▾]"});
}

TEST_CASE("tui lists: an open list closes when the focus leaves it", "[tui][lists][focus]") {
    Harness harness{30, 4};
    auto const row = harness.make<StackImpl>(nullptr, ui::Axis::Horizontal);
    auto const select = harness.make<DropdownSelectImpl>(row.get());
    auto const next = harness.make<ButtonImpl>(row.get());
    next->setLabel("Go");
    int clicks = 0;
    next->setOnClick([&] { ++clicks; });
    int chosen = 0;
    select->setOnSelect([&](ui::Key const&) { ++chosen; });
    select->setOptions(colours());
    select->setSelected(key(1));
    CHECK(harness.draw() == Rows{"[Red ▾]  [ Go ]"});
    harness.focus(*select);
    static_cast<void>(harness.key(KeyCode::Enter));
    REQUIRE(select->isOpen());

    SECTION("Tab is left to the frontend, from the field") {
        CHECK(harness.key(KeyCode::Tab) == EventResult::Ignored);
        CHECK(harness.focused(*select));
    }
    SECTION("a click elsewhere") {
        CHECK(harness.click({.x = 10, .y = 0}) == EventResult::Handled);
        CHECK(clicks == 1);
        CHECK(harness.focused(*next));
    }
    SECTION("the focus moving on") {
        morph::tui::detail::moveFocus(harness.context(), Direction::Forward);
        CHECK(harness.focused(*select));
    }
    CHECK_FALSE(select->isOpen());
    CHECK(chosen == 0);
    CHECK(harness.draw() == Rows{"[Red ▾]  [ Go ]"});
}

TEST_CASE("tui lists: a press on an open list ends a press elsewhere whose release never came",
          "[tui][lists][click]") {
    using Type = core::tui::MouseEvent::Type;
    Harness harness{20, 6};
    auto const column = harness.make<StackImpl>(nullptr, ui::Axis::Vertical);
    auto const panel = harness.make<PanelImpl>(column.get());
    panel->setTitle("Info");
    panel->setCollapsible(true);
    panel->setCollapsed(true);
    auto const select = harness.make<DropdownSelectImpl>(column.get());
    std::vector<bool> toggles;
    panel->setOnToggle([&](bool collapsed) { toggles.push_back(collapsed); });
    std::vector<ui::Key> chosen;
    select->setOnSelect([&](ui::Key picked) { chosen.push_back(std::move(picked)); });
    select->setOptions(colours());
    CHECK(harness.draw() == Rows{"▸ Info", "[ ▾]"});
    harness.focus(*select);
    static_cast<void>(harness.key(KeyCode::Enter));
    REQUIRE(select->isOpen());
    CHECK(harness.draw() == Rows{"▸ Info", "[ ▾]", "▶ Red", "  Green"});
    // A press on the panel's title, which leaves the focus on the list, and whose release is lost.
    CHECK(harness.send(core::tui::MouseEvent{.type = Type::Press, .button = 0, .x = 3, .y = 1}) ==
          EventResult::Handled);
    CHECK(select->isOpen());
    CHECK(harness.click({.x = 3, .y = 3}) == EventResult::Handled);
    CHECK(chosen == std::vector<ui::Key>{key(2)});
    // A release with no press of its own clicks nothing.
    static_cast<void>(harness.send(core::tui::MouseEvent{.type = Type::Release, .button = 0, .x = 3, .y = 1}));
    CHECK(toggles.empty());
}

TEST_CASE("tui lists: a handler run by the focus leaving for an opening list may close or destroy the select",
          "[tui][lists][focus][lifetime]") {
    Harness harness{30, 4};
    auto const column = harness.make<StackImpl>(nullptr, ui::Axis::Vertical);
    auto const picker = harness.make<FilePickerImpl>(column.get(), ui::FilePickerMode::Open);
    auto select = harness.make<DropdownSelectImpl>(column.get());
    select->setOptions(colours());
    int picks = 0;
    SECTION("by taking its options away") {
        picker->setOnPicked([&](std::string const& /*path*/) {
            ++picks;
            select->setOptions({});
        });
    }
    SECTION("by destroying it") {
        picker->setOnPicked([&](std::string const& /*path*/) {
            ++picks;
            select.reset();
        });
    }
    static_cast<void>(harness.draw());
    harness.focus(*picker);
    static_cast<void>(harness.type("/p"));
    WidgetBase::of(*select).activate();
    CHECK(picks == 1);
    CHECK((select == nullptr || !select->isOpen()));
    CHECK(harness.screen().focusedComponent() == nullptr);
    CHECK_FALSE(shows(harness.draw(), "Red"));
}

namespace {

enum class Lock : std::uint8_t { Hidden, Disabled, Collapsed };

}  // namespace

TEST_CASE("tui lists: an open list closes and takes no input once its select cannot be reached",
          "[tui][lists][gating]") {
    auto const lock = GENERATE(Lock::Hidden, Lock::Disabled, Lock::Collapsed);
    Harness harness{20, 8};
    auto const outer = harness.make<StackImpl>(nullptr, ui::Axis::Vertical);
    auto const panel = harness.make<PanelImpl>(outer.get());
    panel->setCollapsible(true);
    auto const inner = harness.make<StackImpl>(panel.get(), ui::Axis::Vertical);
    auto const select = harness.make<DropdownSelectImpl>(inner.get());
    std::vector<ui::Key> chosen;
    select->setOnSelect([&](ui::Key picked) { chosen.push_back(std::move(picked)); });
    select->setOptions(colours());
    select->setSelected(key(1));
    static_cast<void>(harness.draw());
    harness.focus(*select);
    static_cast<void>(harness.key(KeyCode::Enter));
    REQUIRE(select->isOpen());
    REQUIRE(shows(harness.draw(), "Green"));

    auto const gate = [&](bool locked) {
        if (lock == Lock::Hidden) {
            outer->setVisible(!locked);
        } else if (lock == Lock::Disabled) {
            outer->setEnabled(!locked);
        } else {
            panel->setCollapsed(locked);
        }
    };
    gate(true);
    CHECK_FALSE(select->isOpen());
    CHECK_FALSE(shows(harness.draw(), "Green"));
    CHECK(harness.screen().focusedComponent() == nullptr);
    CHECK(harness.key(KeyCode::Down) == EventResult::Ignored);
    CHECK(harness.key(KeyCode::Enter) == EventResult::Ignored);
    CHECK(chosen.empty());

    gate(false);
    CHECK_FALSE(select->isOpen());
    static_cast<void>(harness.draw());
    harness.focus(*select);
    static_cast<void>(harness.key(KeyCode::Enter));
    CHECK(select->isOpen());
    static_cast<void>(harness.key(KeyCode::Down));
    static_cast<void>(harness.key(KeyCode::Enter));
    CHECK(chosen == std::vector<ui::Key>{key(2)});
}

TEST_CASE("tui lists: an open list takes no key once a container around it stops letting its children act",
          "[tui][lists][gating]") {
    Harness harness{20, 4};
    auto const gate = harness.make<Gate>(nullptr);
    auto const inner = harness.make<StackImpl>(gate.get(), ui::Axis::Vertical);
    auto const select = harness.make<DropdownSelectImpl>(inner.get());
    int chosen = 0;
    select->setOnSelect([&](ui::Key const&) { ++chosen; });
    select->setOptions(colours());
    static_cast<void>(harness.draw());
    harness.focus(*select);
    static_cast<void>(harness.key(KeyCode::Enter));
    REQUIRE(select->isOpen());
    gate->setOpen(false);
    CHECK(harness.key(KeyCode::Enter) == EventResult::Ignored);
    CHECK(harness.click({.x = 3, .y = 1}) != EventResult::Handled);
    CHECK(chosen == 0);
    CHECK_FALSE(select->isOpen());
}

TEST_CASE("tui lists: a select, a menu and tabs inside a disabled container two levels up take no key, click or focus",
          "[tui][lists][gating]") {
    Harness harness{20, 8};
    auto const column = harness.make<StackImpl>(nullptr, ui::Axis::Vertical);
    auto const outer = harness.make<StackImpl>(column.get(), ui::Axis::Vertical);
    auto const inner = harness.make<StackImpl>(outer.get(), ui::Axis::Vertical);
    auto const radio = harness.make<RadioSelectImpl>(inner.get());
    auto const dropdown = harness.make<DropdownSelectImpl>(inner.get());
    auto const menu = harness.make<MenuImpl>(inner.get());
    auto const tabs = harness.make<TabsImpl>(inner.get());
    auto const other = harness.make<ButtonImpl>(column.get());
    other->setLabel("Other");
    int calls = 0;
    radio->setOptions(colours());
    radio->setOnSelect([&](ui::Key const&) { ++calls; });
    dropdown->setOptions(colours());
    dropdown->setOnSelect([&](ui::Key const&) { ++calls; });
    menu->setItems({"Open", "Quit"});
    menu->setOnActivate([&](std::size_t /*index*/) { ++calls; });
    tabs->setTabs({"One", "Two"});
    tabs->setSelected(0);
    tabs->setOnSelect([&](std::size_t /*index*/) { ++calls; });
    Rows const shown{"▶ ( ) Red", "  ( ) Green", "[ ▾]", "▶ Open", "  Quit", "[One] Two", "[ Other ]"};
    CHECK(harness.draw() == shown);

    outer->setEnabled(false);
    for (ui::Widget* const gated : {static_cast<ui::Widget*>(radio.get()), static_cast<ui::Widget*>(dropdown.get()),
                                    static_cast<ui::Widget*>(menu.get()), static_cast<ui::Widget*>(tabs.get())}) {
        CHECK_FALSE(WidgetBase::of(*gated).view().focusable());
        harness.focus(*gated);  // focus taken before the container was disabled
        CHECK(harness.key(KeyCode::Enter) == EventResult::Ignored);
        CHECK(harness.type(" ") == EventResult::Ignored);
        CHECK(harness.key(KeyCode::Down) == EventResult::Ignored);
        CHECK(harness.key(KeyCode::Right) == EventResult::Ignored);
        // The primary action itself, however it is reached, does nothing either.
        WidgetBase::of(*gated).activate();
    }
    CHECK_FALSE(dropdown->isOpen());
    harness.screen().setFocus(nullptr);
    for (int const row : {1, 2, 3, 5}) {
        CHECK(harness.click({.x = 3, .y = row}) == EventResult::Ignored);
    }
    CHECK_FALSE(dropdown->isOpen());
    CHECK(calls == 0);
    morph::tui::detail::moveFocus(harness.context(), Direction::Forward);
    CHECK(harness.focused(*other));
    CHECK(harness.draw() == shown);

    outer->setEnabled(true);
    static_cast<void>(harness.draw());
    CHECK(harness.click({.x = 3, .y = 1}) == EventResult::Handled);
    CHECK(calls == 1);
}

TEST_CASE("tui lists: keys a focused page leaves do not switch tabs, and a click on the page body keeps the focus",
          "[tui][lists][focus]") {
    Harness harness{20, 3};
    auto const tabs = harness.make<TabsImpl>(nullptr);
    std::vector<std::size_t> selected;
    tabs->setTabs({"One", "Two"});
    tabs->setSelected(0);
    tabs->setOnSelect([&](std::size_t index) { selected.push_back(index); });
    auto const page = harness.make<ButtonImpl>(tabs.get());
    page->setLabel("Go");
    CHECK(harness.draw() == Rows{"[One] Two", "[ Go ]"});
    harness.focus(*page);
    CHECK(harness.key(KeyCode::Right) == EventResult::Ignored);
    CHECK(harness.key(KeyCode::Left) == EventResult::Ignored);
    CHECK(selected.empty());
    static_cast<void>(harness.click({.x = 10, .y = 2}));
    CHECK(harness.focused(*page));
    CHECK(selected.empty());
}

TEST_CASE("tui lists: a setSelected from inside the tabs' own onSelect stands", "[tui][lists]") {
    Harness harness{20, 2};
    auto const tabs = harness.make<TabsImpl>(nullptr);
    std::vector<std::size_t> selected;
    tabs->setTabs({"One", "Two"});
    tabs->setSelected(0);
    // As the mount does after the new page failed to mount: the bar goes back to the page still shown.
    tabs->setOnSelect([&](std::size_t index) {
        selected.push_back(index);
        tabs->setSelected(0);
    });
    harness.focus(*tabs);
    CHECK(harness.key(KeyCode::Right) == EventResult::Handled);
    CHECK(selected == std::vector<std::size_t>{1});
    CHECK(tabs->probeText() == "One");
    CHECK(harness.draw() == Rows{"[One] Two"});
}

TEST_CASE("tui lists: tabs selected past the last highlight none; Right then picks the first, Left the last",
          "[tui][lists]") {
    Harness harness{20, 2};
    auto const tabs = harness.make<TabsImpl>(nullptr);
    std::vector<std::size_t> selected;
    tabs->setTabs({"One", "Two"});
    tabs->setSelected(2);
    tabs->setOnSelect([&](std::size_t index) { selected.push_back(index); });
    CHECK(harness.draw() == Rows{" One  Two"});
    CHECK(tabs->probeText().empty());
    harness.focus(*tabs);
    static_cast<void>(harness.key(KeyCode::Right));
    tabs->setSelected(7);
    static_cast<void>(harness.key(KeyCode::Left));
    CHECK(selected == std::vector<std::size_t>{0, 1});
    CHECK(harness.draw() == Rows{" One [Two]"});
}

TEST_CASE("tui lists: a list handler may destroy its own widget", "[tui][lists][lifetime]") {
    Harness harness{20, 4};
    auto const column = harness.make<StackImpl>(nullptr, ui::Axis::Vertical);
    auto const survivor = harness.make<ButtonImpl>(column.get());
    survivor->setLabel("Stay");
    int calls = 0;

    SECTION("a radio select's onSelect, by Space") {
        auto radio = harness.make<RadioSelectImpl>(column.get());
        radio->setOptions(colours());
        radio->setOnSelect([&radio, &calls](ui::Key const& /*key*/) {
            radio.reset();
            ++calls;
        });
        harness.focus(*radio);
        CHECK(harness.type(" ") == EventResult::Handled);
        CHECK(radio == nullptr);
    }
    SECTION("a radio select's onSelect, by a click") {
        auto radio = harness.make<RadioSelectImpl>(column.get());
        radio->setOptions(colours());
        radio->setOnSelect([&radio, &calls](ui::Key const& /*key*/) {
            radio.reset();
            ++calls;
        });
        static_cast<void>(harness.draw());
        CHECK(harness.click({.x = 3, .y = 2}) == EventResult::Handled);
        CHECK(radio == nullptr);
    }
    SECTION("a dropdown's onSelect, by Enter in its list") {
        auto dropdown = harness.make<DropdownSelectImpl>(column.get());
        dropdown->setOptions(colours());
        dropdown->setOnSelect([&dropdown, &calls](ui::Key const& /*key*/) {
            dropdown.reset();
            ++calls;
        });
        static_cast<void>(harness.draw());
        harness.focus(*dropdown);
        static_cast<void>(harness.key(KeyCode::Enter));
        CHECK(harness.key(KeyCode::Enter) == EventResult::Handled);
        CHECK(dropdown == nullptr);
    }
    SECTION("a dropdown's onSelect, by a click in its list") {
        auto dropdown = harness.make<DropdownSelectImpl>(column.get());
        dropdown->setOptions(colours());
        dropdown->setOnSelect([&dropdown, &calls](ui::Key const& /*key*/) {
            dropdown.reset();
            ++calls;
        });
        static_cast<void>(harness.draw());
        CHECK(harness.click({.x = 1, .y = 1}) == EventResult::Handled);
        static_cast<void>(harness.draw());
        CHECK(harness.click({.x = 3, .y = 3}) == EventResult::Handled);
        CHECK(dropdown == nullptr);
    }
    SECTION("a menu's onActivate, by Enter") {
        auto menu = harness.make<MenuImpl>(column.get());
        menu->setItems({"Open", "Quit"});
        menu->setOnActivate([&menu, &calls](std::size_t /*index*/) {
            menu.reset();
            ++calls;
        });
        harness.focus(*menu);
        CHECK(harness.key(KeyCode::Enter) == EventResult::Handled);
        CHECK(menu == nullptr);
    }
    SECTION("a menu's onActivate, by a click") {
        auto menu = harness.make<MenuImpl>(column.get());
        menu->setItems({"Open", "Quit"});
        menu->setOnActivate([&menu, &calls](std::size_t /*index*/) {
            menu.reset();
            ++calls;
        });
        static_cast<void>(harness.draw());
        CHECK(harness.click({.x = 3, .y = 2}) == EventResult::Handled);
        CHECK(menu == nullptr);
    }
    SECTION("tabs' onSelect, by Right") {
        auto tabs = harness.make<TabsImpl>(column.get());
        tabs->setTabs({"One", "Two"});
        tabs->setOnSelect([&tabs, &calls](std::size_t /*index*/) {
            tabs.reset();
            ++calls;
        });
        harness.focus(*tabs);
        CHECK(harness.key(KeyCode::Right) == EventResult::Handled);
        CHECK(tabs == nullptr);
    }
    SECTION("tabs' onSelect, by a click") {
        auto tabs = harness.make<TabsImpl>(column.get());
        tabs->setTabs({"One", "Two"});
        tabs->setOnSelect([&tabs, &calls](std::size_t /*index*/) {
            tabs.reset();
            ++calls;
        });
        static_cast<void>(harness.draw());
        CHECK(harness.click({.x = 7, .y = 1}) == EventResult::Handled);
        CHECK(tabs == nullptr);
    }

    CHECK(calls == 1);
    // The backend stays usable: the survivor still draws and takes a click.
    int survivorClicks = 0;
    survivor->setOnClick([&] { ++survivorClicks; });
    CHECK(harness.draw() == Rows{"[ Stay ]"});
    CHECK(harness.click({.x = 1, .y = 0}) == EventResult::Handled);
    CHECK(survivorClicks == 1);
}

TEST_CASE("tui lists: no list handler runs after its widget was destroyed", "[tui][lists][lifetime]") {
    Harness harness{20, 4};
    auto const column = harness.make<StackImpl>(nullptr, ui::Axis::Vertical);
    int calls = 0;
    SECTION("a radio select") {
        auto radio = harness.make<RadioSelectImpl>(column.get());
        radio->setOptions(colours());
        radio->setOnSelect([&](ui::Key const&) { ++calls; });
        static_cast<void>(harness.draw());
        harness.focus(*radio);
        radio.reset();
    }
    SECTION("a dropdown with its list open") {
        auto dropdown = harness.make<DropdownSelectImpl>(column.get());
        dropdown->setOptions(colours());
        dropdown->setOnSelect([&](ui::Key const&) { ++calls; });
        static_cast<void>(harness.draw());
        harness.focus(*dropdown);
        static_cast<void>(harness.key(KeyCode::Enter));
        REQUIRE(dropdown->isOpen());
        REQUIRE(shows(harness.draw(), "Green"));
        dropdown.reset();
        CHECK(harness.draw().empty());
    }
    SECTION("a menu") {
        auto menu = harness.make<MenuImpl>(column.get());
        menu->setItems({"Open", "Quit"});
        menu->setOnActivate([&](std::size_t /*index*/) { ++calls; });
        static_cast<void>(harness.draw());
        harness.focus(*menu);
        menu.reset();
    }
    SECTION("tabs") {
        auto tabs = harness.make<TabsImpl>(column.get());
        tabs->setTabs({"One", "Two"});
        tabs->setOnSelect([&](std::size_t /*index*/) { ++calls; });
        static_cast<void>(harness.draw());
        harness.focus(*tabs);
        tabs.reset();
    }
    CHECK(harness.screen().focusedComponent() == nullptr);
    CHECK(harness.key(KeyCode::Enter) == EventResult::Ignored);
    CHECK(harness.type(" ") == EventResult::Ignored);
    CHECK(harness.key(KeyCode::Right) == EventResult::Ignored);
    CHECK(harness.click({.x = 3, .y = 1}) == EventResult::Ignored);
    CHECK(calls == 0);
}

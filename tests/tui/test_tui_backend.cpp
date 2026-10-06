// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <core/net/PlatformLoop.hpp>
#include <core/tui/InputEvent.hpp>
#include <core/tui/KeyCode.hpp>
#include <core/tui/MockTerminalOutput.hpp>
#include <core/tui/Rect.hpp>
#include <core/tui/Screen.hpp>
#include <core/tui/Terminal.hpp>
#include <core/tui/TestHelpers.hpp>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/signal.hpp>
#include <morph/tui/backend.hpp>
#include <morph/tui/loop_executor.hpp>
#include <morph/ui/backend.hpp>
#include <morph/ui/mount.hpp>
#include <morph/ui/view.hpp>
#include <morph/util/datetime.hpp>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "tui/list_widgets.hpp"
#include "tui/table_widget.hpp"
#include "tui/widget.hpp"
#include "tui_harness.hpp"

namespace ui = morph::ui;
using core::tui::KeyCode;
using morph::reactive::Runtime;
using morph::reactive::Signal;
using morph::tui::Backend;
using morph::tui::detail::ContainerBase;
using morph::tui::detail::DropdownSelectImpl;
using morph::tui::detail::RadioSelectImpl;
using morph::tui::detail::TableImpl;
using morph::tui::detail::WidgetBase;
using morph::tui::testing::ResizableOutput;
using morph::tui::testing::rowsOf;
using namespace std::chrono_literals;

namespace {

/// A terminal, a screen over it and a backend over that.
struct Stage {
    explicit Stage(std::unique_ptr<core::tui::TerminalOutput> output)
        : terminal{std::move(output)}, screen{terminal}, backend{screen} {}
    core::tui::Terminal terminal;
    core::tui::Screen screen;
    Backend backend;
};

/// What the TUI frontend runs a mounted view on: a backend on a headless screen, and a reactive runtime owned by a
/// loop executor on a loop the test turns.
struct Live {
    explicit Live(core::tui::Size size, Backend::Clock now = {})
        : terminal{std::make_unique<core::tui::MockTerminalOutput>(size.width, size.height)},
          screen{terminal},
          backend{screen, std::move(now)} {}

    /// Runs the posted flushes, then fits and draws a frame, as the frontend does after every input event.
    void settle() {
        static_cast<void>(loop.runUntilIdle());
        backend.fit();
        screen.draw();
    }

    void key(KeyCode code) {
        static_cast<void>(screen.dispatchEvent(core::tui::test::specialKey(code)));
        settle();
    }

    void type(char character) {
        static_cast<void>(screen.dispatchEvent(core::tui::test::charKey(character)));
        settle();
    }

    /// A left press and release on @p cell, a 0-based viewport cell.
    void click(core::tui::Point cell) {
        using Type = core::tui::MouseEvent::Type;
        static_cast<void>(screen.dispatchEvent(
            core::tui::MouseEvent{.type = Type::Press, .button = 0, .x = cell.x + 1, .y = cell.y + 1}));
        static_cast<void>(screen.dispatchEvent(
            core::tui::MouseEvent{.type = Type::Release, .button = 0, .x = cell.x + 1, .y = cell.y + 1}));
        settle();
    }

    [[nodiscard]] bool focused(ui::Widget const& widget) const {
        return screen.focusedComponent() == &WidgetBase::of(widget).view();
    }

    core::net::PlatformLoop loop;
    morph::tui::LoopExecutor executor{loop};
    Runtime runtime{executor};
    core::tui::Terminal terminal;
    core::tui::Screen screen;
    Backend backend;
};

/// The child at @p index of the container @p widget.
ui::Widget& childOf(ui::Widget const& widget, std::size_t index) {
    auto const children = ContainerBase::of(dynamic_cast<ui::ContainerWidget const&>(widget)).children();
    REQUIRE(index < children.size());
    return dynamic_cast<ui::Widget&>(**std::next(children.begin(), static_cast<std::ptrdiff_t>(index)));
}

/// The cell in the middle of where @p widget was drawn last.
core::tui::Point middleOf(ui::Widget const& widget) {
    auto const bounds = WidgetBase::of(widget).view().screenBounds();
    return {.x = bounds.x + (bounds.width / 2), .y = bounds.y + (bounds.height / 2)};
}

ui::Key intKey(std::int64_t value) { return ui::Key{value}; }

}  // namespace

TEST_CASE("tui::Backend: every factory appends one widget to its parent", "[tui][backend]") {
    Stage stage{std::make_unique<core::tui::MockTerminalOutput>(40, 12)};
    auto& backend = stage.backend;
    auto const column = backend.createStack(nullptr, ui::Axis::Vertical);
    std::vector<std::unique_ptr<ui::Widget>> made;
    made.push_back(backend.createText(column.get()));
    made.push_back(backend.createButton(column.get()));
    made.push_back(backend.createTextInput(column.get(), ui::TextInputMode::Multiline));
    made.push_back(backend.createCheckbox(column.get()));
    made.push_back(backend.createSelect(column.get(), ui::SelectStyle::Radio));
    made.push_back(backend.createSelect(column.get(), ui::SelectStyle::Dropdown));
    made.push_back(backend.createMenu(column.get()));
    made.push_back(backend.createStack(column.get(), ui::Axis::Horizontal));
    made.push_back(backend.createGrid(column.get()));
    made.push_back(backend.createSpacer(column.get()));
    made.push_back(backend.createPanel(column.get()));
    made.push_back(backend.createScroll(column.get(), ui::Axis::Vertical));
    made.push_back(backend.createSlot(column.get()));
    made.push_back(backend.createTabs(column.get()));
    made.push_back(backend.createDialog(column.get()));
    made.push_back(backend.createBusy(column.get()));
    made.push_back(backend.createTable(column.get()));
    made.push_back(backend.createDateTimeInput(column.get(), ui::DateMode::Date, 0));
    made.push_back(backend.createSlider(column.get()));
    made.push_back(backend.createFilePicker(column.get(), ui::FilePickerMode::Save));
    auto const& container = ContainerBase::of(*column);
    CHECK(container.children().size() == made.size());
    std::size_t index = 0;
    for (auto const* child : container.children()) {
        CHECK(&child->asWidget() == made.at(index).get());
        ++index;
    }
    backend.fit();
    stage.screen.draw();
    made.clear();
    CHECK(ContainerBase::of(*column).children().empty());
}

TEST_CASE("tui::Backend: every factory hands its parameters to the widget it makes", "[tui][backend]") {
    Stage stage{std::make_unique<core::tui::MockTerminalOutput>(30, 8)};
    auto& backend = stage.backend;
    auto const column = backend.createStack(nullptr, ui::Axis::Vertical);
    auto const row = backend.createStack(column.get(), ui::Axis::Horizontal);
    auto const left = backend.createText(row.get());
    auto const right = backend.createText(row.get());
    left->setText("a");
    right->setText("b");
    auto const date = backend.createDateTimeInput(column.get(), ui::DateMode::DateTime, 60);
    date->setValue(morph::time::Timestamp{morph::time::DateTime{std::chrono::year{2026}, std::chrono::month{10},
                                                                std::chrono::day{4}, std::chrono::hours{13},
                                                                std::chrono::minutes{5}, std::chrono::seconds{0}}});
    auto const picker = backend.createFilePicker(column.get(), ui::FilePickerMode::Save);
    auto const multiline = backend.createTextInput(column.get(), ui::TextInputMode::Multiline);
    auto const radio = backend.createSelect(column.get(), ui::SelectStyle::Radio);
    auto const dropdown = backend.createSelect(column.get(), ui::SelectStyle::Dropdown);
    backend.fit();
    stage.screen.draw();
    auto const rows = rowsOf(stage.screen);
    REQUIRE(rows.size() >= 3);
    CHECK(rows.at(0) == "ab");
    CHECK(rows.at(1) == "2026-10-04 14:05");
    CHECK(rows.at(2) == "Save:");
    CHECK(WidgetBase::of(*multiline).naturalSize().height == 3);
    CHECK(dynamic_cast<RadioSelectImpl const*>(radio.get()) != nullptr);
    CHECK(dynamic_cast<DropdownSelectImpl const*>(dropdown.get()) != nullptr);
}

TEST_CASE("tui::Backend: fit re-fits the root to the screen after a resize", "[tui][backend]") {
    auto output = std::make_unique<ResizableOutput>(core::tui::Size{.width = 12, .height = 4});
    auto* const terminalOutput = output.get();
    Stage stage{std::move(output)};
    auto const panel = stage.backend.createPanel(nullptr);
    panel->setTitle("Box");
    stage.backend.fit();
    stage.screen.draw();
    CHECK(rowsOf(stage.screen).back() == "└──────────┘");

    terminalOutput->resize({.width = 16, .height = 5});
    static_cast<void>(stage.screen.dispatchEvent(core::tui::ResizeEvent{.columns = 16, .rows = 5}));
    stage.backend.fit();
    stage.screen.draw();
    auto const rows = rowsOf(stage.screen);
    CHECK(rows.size() == 5);
    CHECK(rows.back() == "└──────────────┘");
}

TEST_CASE("tui::Backend: animating while a Busy spins; advanceAnimation moves its frame", "[tui][backend]") {
    Stage stage{std::make_unique<core::tui::MockTerminalOutput>(20, 1)};
    auto const busy = stage.backend.createBusy(nullptr);
    busy->setLabel("Wait");
    CHECK_FALSE(stage.backend.animating());
    busy->setActive(true);
    CHECK(stage.backend.animating());
    stage.backend.fit();
    stage.screen.draw();
    CHECK(rowsOf(stage.screen) == std::vector<std::string>{"| Wait"});
    stage.backend.advanceAnimation();
    stage.screen.draw();
    CHECK(rowsOf(stage.screen) == std::vector<std::string>{"/ Wait"});
}

TEST_CASE("tui::Backend: focusFirst focuses the first focusable widget only when nothing has focus",
          "[tui][backend]") {
    Stage stage{std::make_unique<core::tui::MockTerminalOutput>(20, 3)};
    auto const column = stage.backend.createStack(nullptr, ui::Axis::Vertical);
    auto const label = stage.backend.createText(column.get());
    auto const first = stage.backend.createButton(column.get());
    auto const second = stage.backend.createButton(column.get());
    stage.backend.focusFirst();
    CHECK(stage.screen.focusedComponent() == &morph::tui::detail::WidgetBase::of(*first).view());
    stage.backend.focusNext();
    stage.backend.focusFirst();
    CHECK(stage.screen.focusedComponent() == &morph::tui::detail::WidgetBase::of(*second).view());
    stage.backend.focusPrev();
    CHECK(stage.screen.focusedComponent() == &morph::tui::detail::WidgetBase::of(*first).view());
}

TEST_CASE("tui::Backend: focusPrev walks back through morph's order and wraps at the start", "[tui][backend]") {
    Stage stage{std::make_unique<core::tui::MockTerminalOutput>(20, 3)};
    auto const column = stage.backend.createStack(nullptr, ui::Axis::Vertical);
    auto const first = stage.backend.createButton(column.get());
    auto const second = stage.backend.createButton(column.get());
    auto const third = stage.backend.createButton(column.get());
    column->moveChild(*third, 0);
    auto const focused = [&stage](ui::Widget const& widget) {
        return stage.screen.focusedComponent() == &WidgetBase::of(widget).view();
    };
    stage.backend.focusFirst();
    CHECK(focused(*third));
    stage.backend.focusPrev();
    CHECK(focused(*second));
    stage.backend.focusPrev();
    CHECK(focused(*first));
    stage.backend.focusPrev();
    CHECK(focused(*third));
}

TEST_CASE("tui::Backend: a scroll made horizontal scrolls across", "[tui][backend]") {
    auto const axis = GENERATE(ui::Axis::Horizontal, ui::Axis::Vertical);
    Stage stage{std::make_unique<core::tui::MockTerminalOutput>(6, 1)};
    auto const scroll = stage.backend.createScroll(nullptr, axis);
    auto const text = stage.backend.createText(scroll.get());
    text->setText("abcdefghij");
    stage.backend.fit();
    stage.screen.draw();
    stage.backend.focusFirst();
    static_cast<void>(stage.screen.dispatchEvent(core::tui::test::specialKey(KeyCode::Right)));
    stage.backend.fit();
    stage.screen.draw();
    auto const* const shown = axis == ui::Axis::Horizontal ? "bcdefg" : "abcdef";
    CHECK(rowsOf(stage.screen) == std::vector<std::string>{shown});
}

TEST_CASE("tui::Backend: focusFirst moves into an open dialog whose content arrived after it opened",
          "[tui][backend]") {
    Stage stage{std::make_unique<core::tui::MockTerminalOutput>(30, 10)};
    auto const column = stage.backend.createStack(nullptr, ui::Axis::Vertical);
    auto const dialog = stage.backend.createDialog(column.get());
    dialog->setOpen(true);  // nothing inside yet: the frame holds the focus
    auto const body = stage.backend.createStack(dialog.get(), ui::Axis::Vertical);
    auto const okButton = stage.backend.createButton(body.get());
    stage.backend.focusFirst();
    CHECK(stage.screen.focusedComponent() == &morph::tui::detail::WidgetBase::of(*okButton).view());
}

TEST_CASE("tui::Backend: the clock it is given is the one a table measures a double click on", "[tui][backend]") {
    auto now = std::chrono::steady_clock::time_point{};
    Live live{{.width = 30, .height = 6}, [&now] { return now; }};
    Signal<std::vector<std::int64_t>> rows{live.runtime, {1, 2, 3}};
    std::vector<ui::Key> activated;
    ui::Mounted const view{live.runtime, live.backend,
                           ui::table<std::int64_t>(
                               {{.label = "Name"}}, [&rows] { return rows.get(); }, intKey,
                               [](Signal<std::int64_t> const& row) {
                                   return std::vector<ui::Node>{
                                       ui::text({.text = [&row] { return "row " + std::to_string(row.get()); }})};
                               },
                               {.selectionMode = ui::SelectionMode::Single,
                                .onActivate = [&activated](ui::Key key) { activated.push_back(std::move(key)); }})};
    live.settle();
    auto const second = middleOf(childOf(view.root(), 1));
    live.click(second);
    now += 1s;
    live.click(second);
    CHECK(activated.empty());
    live.click(second);
    CHECK(activated == std::vector<ui::Key>{intKey(2)});
}

TEST_CASE("tui::Backend: no widget of a mounted view is left as the hover target once it is gone",
          "[tui][backend][hover]") {
    Stage stage{std::make_unique<core::tui::MockTerminalOutput>(20, 6)};
    stage.screen.hoverState().setDelay(0ms);
    using Type = core::tui::MouseEvent::Type;

    SECTION("a widget's own view") {
        auto button = stage.backend.createButton(nullptr);
        button->setLabel("Hover");
        stage.backend.fit();
        stage.screen.draw();
        static_cast<void>(stage.screen.dispatchEvent(core::tui::MouseEvent{.type = Type::Move, .x = 2, .y = 1}));
        REQUIRE(stage.screen.hoverState().currentHover().has_value());
        button.reset();
    }
    SECTION("a popup the widget owns") {
        auto select = stage.backend.createSelect(nullptr, ui::SelectStyle::Dropdown);
        select->setOptions(
            std::vector<ui::SelectOption>{{.key = intKey(1), .label = "One"}, {.key = intKey(2), .label = "Two"}});
        stage.backend.fit();
        stage.screen.draw();
        stage.backend.focusFirst();
        static_cast<void>(stage.screen.dispatchEvent(core::tui::test::specialKey(KeyCode::Enter)));
        stage.backend.fit();
        stage.screen.draw();
        auto const* const popup = stage.screen.focusedComponent();
        REQUIRE(popup != nullptr);
        REQUIRE(popup != &WidgetBase::of(*select).view());
        auto const bounds = popup->screenBounds();
        static_cast<void>(stage.screen.dispatchEvent(
            core::tui::MouseEvent{.type = Type::Move, .x = bounds.x + 1, .y = bounds.y + 1}));
        REQUIRE(stage.screen.hoverState().currentHover().has_value());
        REQUIRE(stage.screen.hoverState().currentHover()->target == popup);
        select.reset();
    }
    // With a destroyed target left behind, the hover timer would call it.
    CHECK_FALSE(stage.screen.hoverState().currentHover().has_value());
    stage.screen.tickHover();
}

TEST_CASE("tui::Backend: a Switch inside an open Dialog, focused through the frame, and the focus handed back",
          "[tui][backend][mount]") {
    Live live{{.width = 40, .height = 12}};
    Signal<bool> open{live.runtime, false};
    Signal<std::int64_t> page{live.runtime, 0};
    int saved = 0;
    std::optional<ui::Mounted> view;
    view.emplace(live.runtime, live.backend,
                 ui::column({.children = {
                                 ui::button({.label = "Open", .onClick = [&open] { open.set(true); }}),
                                 ui::dialog({.open = [&open] { return open.get(); },
                                             .title = "Ask",
                                             .child = ui::switchOf({.selector = [&page] { return intKey(page.get()); },
                                                                    .cases = {{.key = intKey(1),
                                                                               .node = ui::button({.label = "Save",
                                                                                                   .onClick =
                                                                                                       [&] {
                                                                                                           ++saved;
                                                                                                           open.set(
                                                                                                               false);
                                                                                                       }})}}}),
                                             .onDismiss = [&open] { open.set(false); }}),
                             }}));
    auto& opener = childOf(view->root(), 0);
    auto& frame = ContainerBase::of(dynamic_cast<ui::ContainerWidget&>(childOf(view->root(), 1))).host();
    live.settle();
    live.backend.focusFirst();
    REQUIRE(live.focused(opener));

    // Opened with nothing focusable inside: the frame holds the focus, and keeps it until the content arrives.
    live.key(KeyCode::Enter);
    CHECK(live.screen.focusedComponent() == &frame);
    live.backend.focusFirst();
    CHECK(live.screen.focusedComponent() == &frame);
    page.set(1);
    live.settle();
    CHECK(live.screen.focusedComponent() == &frame);
    live.backend.focusFirst();
    auto& save = childOf(childOf(childOf(view->root(), 1), 0), 0);
    CHECK(live.focused(save));

    // Its button closes it: the content, the focused button among it, is destroyed, and the opener has the focus.
    live.key(KeyCode::Enter);
    CHECK(saved == 1);
    CHECK(live.focused(opener));

    // Tab stays inside while it is open; the whole view may go while it is.
    live.key(KeyCode::Enter);
    REQUIRE(live.screen.focusedComponent() != &WidgetBase::of(opener).view());
    live.backend.focusNext();
    live.backend.focusNext();
    CHECK_FALSE(live.focused(opener));
    view.reset();
    live.settle();
    CHECK(live.screen.focusedComponent() == nullptr);
    live.backend.focusFirst();
    CHECK(live.screen.focusedComponent() == nullptr);
}

TEST_CASE("tui::Backend: Tabs keep only the selected page reachable, and a spinner on a hidden page stops",
          "[tui][backend][mount]") {
    Live live{{.width = 30, .height = 6}};
    Signal<std::size_t> selected{live.runtime, 1};
    int clicks = 0;
    ui::Mounted const view{
        live.runtime, live.backend,
        ui::tabs(
            {.tabs = {{.label = "One", .node = ui::button({.label = "First", .onClick = [&clicks] { ++clicks; }})},
                      {.label = "Two", .node = ui::busy({.active = true, .label = "Wait"})}},
             .selected = [&selected] { return selected.get(); },
             .onSelect = [&selected](std::size_t index) { selected.set(index); }})};
    live.settle();
    CHECK(live.backend.animating());
    live.backend.focusFirst();
    REQUIRE(live.focused(view.root()));

    live.key(KeyCode::Left);
    CHECK(selected.get() == 0);
    CHECK_FALSE(live.backend.animating());
    auto& first = childOf(childOf(view.root(), 1), 0);
    auto const where = middleOf(first);
    live.click(where);
    CHECK(clicks == 1);

    // The page that held the button is hidden: where it was drawn takes no click.
    live.backend.focusFirst();
    live.backend.focusPrev();
    REQUIRE(live.focused(view.root()));
    live.key(KeyCode::Right);
    CHECK(selected.get() == 1);
    CHECK(live.backend.animating());
    live.click(where);
    CHECK(clicks == 1);
}

TEST_CASE("tui::Backend: a ForEach row's button removing its own row; focusFirst then finds the next one",
          "[tui][backend][mount]") {
    Live live{{.width = 30, .height = 6}};
    Signal<std::vector<std::int64_t>> rows{live.runtime, {1, 2, 3}};
    ui::Mounted const view{live.runtime, live.backend,
                           ui::forEach<std::int64_t>(rows, intKey, [&rows](Signal<std::int64_t> const& row) {
                               return ui::button({.label = [&row] { return "Drop " + std::to_string(row.get()); },
                                                  .onClick =
                                                      [&rows, &row] {
                                                          auto kept = rows.get();
                                                          std::erase(kept, row.get());
                                                          rows.set(std::move(kept));
                                                      }});
                           })};
    live.settle();
    live.backend.focusFirst();
    live.backend.focusNext();
    REQUIRE(live.focused(childOf(view.root(), 1)));
    live.key(KeyCode::Enter);
    CHECK(rows.get() == std::vector<std::int64_t>{1, 3});
    CHECK(ContainerBase::of(dynamic_cast<ui::ContainerWidget const&>(view.root())).children().size() == 2);
    CHECK(live.screen.focusedComponent() == nullptr);
    live.backend.focusFirst();
    CHECK(live.focused(childOf(view.root(), 0)));
    live.click(middleOf(childOf(view.root(), 1)));
    CHECK(rows.get() == std::vector<std::int64_t>{1});
}

TEST_CASE("tui::Backend: a reordered ForEach keeps a field's typed text and focus, and Tab follows the new order",
          "[tui][backend][mount]") {
    Live live{{.width = 30, .height = 6}};
    Signal<std::vector<std::int64_t>> rows{live.runtime, {1, 2}};
    ui::Mounted const view{live.runtime, live.backend,
                           ui::forEach<std::int64_t>(
                               rows, intKey, [](Signal<std::int64_t> const& /*row*/) { return ui::textInput({}); })};
    live.settle();
    auto& second = childOf(view.root(), 1);
    live.backend.focusFirst();
    live.backend.focusNext();
    REQUIRE(live.focused(second));
    live.type('a');
    live.type('b');

    rows.set({2, 1});
    live.settle();
    CHECK(&childOf(view.root(), 0) == &second);
    CHECK(WidgetBase::of(second).probeText() == "ab");
    CHECK(live.focused(second));
    live.backend.focusNext();
    CHECK(live.focused(childOf(view.root(), 1)));
    live.backend.focusNext();
    CHECK(live.focused(second));
}

TEST_CASE("tui::Backend: a mounted Table marks a selected row again when it returns", "[tui][backend][mount]") {
    Live live{{.width = 30, .height = 6}};
    Signal<std::vector<std::int64_t>> rows{live.runtime, {1, 2, 3}};
    Signal<std::vector<ui::Key>> selection{live.runtime, {}};
    std::vector<std::vector<ui::Key>> reports;
    ui::Mounted const view{live.runtime, live.backend,
                           ui::table<std::int64_t>(
                               {{.label = "Name"}}, [&rows] { return rows.get(); }, intKey,
                               [](Signal<std::int64_t> const& row) {
                                   return std::vector<ui::Node>{
                                       ui::text({.text = [&row] { return "row " + std::to_string(row.get()); }})};
                               },
                               {.selectionMode = ui::SelectionMode::Single,
                                .selection = [&selection] { return selection.get(); },
                                .onSelectionChange =
                                    [&](std::vector<ui::Key> keys) {
                                        reports.push_back(keys);
                                        selection.set(std::move(keys));
                                    }})};
    live.settle();
    auto const& table = dynamic_cast<TableImpl const&>(WidgetBase::of(view.root()));
    live.click(middleOf(childOf(view.root(), 1)));
    CHECK(reports == std::vector<std::vector<ui::Key>>{{intKey(2)}});
    CHECK(table.markedRows() == std::vector<std::size_t>{1});

    rows.set({1, 3});
    live.settle();
    CHECK(table.markedRows().empty());
    rows.set({2, 1, 3});
    live.settle();
    CHECK(table.markedRows() == std::vector<std::size_t>{0});
    CHECK(reports.size() == 1);
}

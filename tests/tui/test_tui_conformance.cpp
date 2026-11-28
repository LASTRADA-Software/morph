// SPDX-License-Identifier: Apache-2.0
//
// The backend conformance suite (docs/spec/ui/backend_contract.md, "The conformance suite") run against the terminal
// backend. The probe drives it as a user would, through core::tui input events dispatched to the screen, the way
// the frontend's session does: mouse presses and releases on the cells a widget was drawn in, keys to the focused
// widget, a paste for files dropped on a terminal. It reads back what the widgets show.

#include <libunicode/convert.h>

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <core/async/ExecutorContext.hpp>
#include <core/tui/InputEvent.hpp>
#include <core/tui/KeyCode.hpp>
#include <core/tui/MockTerminalOutput.hpp>
#include <core/tui/Modifier.hpp>
#include <core/tui/Rect.hpp>
#include <core/tui/Screen.hpp>
#include <core/tui/Terminal.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <morph/reactive/runtime.hpp>
#include <morph/testing/step_executor.hpp>
#include <morph/tui/backend.hpp>
#include <morph/ui/backend.hpp>
#include <morph/ui/testing/backend_conformance.hpp>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "tui/chord.hpp"
#include "tui/container_widgets.hpp"
#include "tui/display_widgets.hpp"
#include "tui/field_widgets.hpp"
#include "tui/list_widgets.hpp"
#include "tui/region_widgets.hpp"
#include "tui/table_widget.hpp"
#include "tui/widget.hpp"
#include "tui_harness.hpp"

namespace ui = morph::ui;
using core::tui::KeyCode;
using core::tui::Modifier;
using morph::tui::detail::WidgetBase;

namespace {

// How a probe breaks the contract on purpose. A suite that passes the terminal backend measures something only if
// it fails a probe that drops the input or misreads the screen.
enum class Fault : std::uint8_t {
    None,       // the terminal probe
    DeadInput,  // no input reaches the screen
    WrongText,  // every widget reads as showing other text
};

using Type = core::tui::MouseEvent::Type;

class TuiProbe final : public ui::testing::ConformanceProbe {
public:
    explicit TuiProbe(Fault fault = Fault::None) : _fault{fault} {}

    ui::IViewBackend& backend() override { return _backend; }
    morph::reactive::Runtime& runtime() override { return _runtime; }

    // As the frontend's session does between events: the posted flushes, then a fitted frame, with the focus given
    // to new content when nothing holds it; again while a frame posts more work.
    void settle() override {
        for (int round = 0; round < 100; ++round) {
            bool const ran = _owner.runAll() > 0;
            core::async::ExecutorScope const scope{_owner.coreExecutor()};
            _backend.fit();
            _backend.focusFirst();
            _screen.draw();
            if (!ran) {
                return;
            }
        }
    }

    [[nodiscard]] std::string textOf(ui::Widget const& widget) override {
        return _fault == Fault::WrongText ? "?" : WidgetBase::of(widget).probeText();
    }
    [[nodiscard]] std::optional<std::string> descriptionOf(ui::Widget const& widget) override {
        return WidgetBase::of(widget).accessibleDescription();
    }
    [[nodiscard]] bool visibleOf(ui::Widget const& widget) override { return WidgetBase::of(widget).userVisible(); }
    [[nodiscard]] bool enabledOf(ui::Widget const& widget) override { return WidgetBase::of(widget).enabled(); }

    [[nodiscard]] std::size_t childCount(ui::ContainerWidget const& container) override {
        return containerOf(container).children().size();
    }
    [[nodiscard]] ui::Widget* childAt(ui::ContainerWidget const& container, std::size_t index) override {
        auto const children = containerOf(container).children();
        if (index >= children.size()) {
            return nullptr;
        }
        // The probe drives the widget it hands out; the backend made it mutable.
        return const_cast<ui::Widget*>(&children[index]->asWidget());  // NOLINT(cppcoreguidelines-pro-type-const-cast)
    }

    void click(ui::Widget& widget) override {
        settle();
        if (auto const cell = drawnCell(widget)) {
            send(core::tui::MouseEvent{.type = Type::Press, .button = 0, .x = cell->x + 1, .y = cell->y + 1});
            send(core::tui::MouseEvent{.type = Type::Release, .button = 0, .x = cell->x + 1, .y = cell->y + 1});
        }
        settle();
    }

    // A click at the horizontal middle of one widget's drawn bounds and the vertical middle of another's.
    void clickAt(ui::Widget& across, ui::Widget& along) override {
        settle();
        auto const column = WidgetBase::of(across).view().screenBounds();
        auto const line = WidgetBase::of(along).view().screenBounds();
        if (column.width > 0 && line.height > 0) {
            int const x = column.x + (column.width / 2);
            int const y = line.y + (line.height / 2);
            send(core::tui::MouseEvent{.type = Type::Press, .button = 0, .x = x + 1, .y = y + 1});
            send(core::tui::MouseEvent{.type = Type::Release, .button = 0, .x = x + 1, .y = y + 1});
        }
        settle();
    }

    // Select all (Ctrl+Shift+A), then the text pasted over the selection, so the field takes it as one edit; an
    // empty text deletes the selection.
    void type(ui::Widget& widget, std::string_view text) override {
        focusByClick(widget);
        send(key(core::tui::KeyEvent{.key = core::tui::keyCodeFromCodepoint(U'a'),
                                     .modifiers = Modifier::Ctrl | Modifier::Shift,
                                     .codepoint = U'a'}));
        if (text.empty()) {
            send(special(KeyCode::Backspace));
        } else {
            send(core::tui::PasteEvent{.text = std::string{text}});
        }
        settle();
    }

    // A press on the source, a move a cell along and then onto the target, and the release there.
    void drag(ui::Widget& source, ui::Widget& target) override {
        settle();
        auto const from = drawnCell(source);
        auto const to = drawnCell(target);
        if (!from || !to) {
            return;
        }
        send(core::tui::MouseEvent{.type = Type::Press, .button = 0, .x = from->x + 1, .y = from->y + 1});
        send(core::tui::MouseEvent{.type = Type::Move, .button = 0, .x = from->x + 2, .y = from->y + 1});
        settle();
        send(core::tui::MouseEvent{.type = Type::Move, .button = 0, .x = to->x + 1, .y = to->y + 1});
        settle();
        send(core::tui::MouseEvent{.type = Type::Release, .button = 0, .x = to->x + 1, .y = to->y + 1});
        settle();
    }

    // Esc: on a banner, which the probe focuses first; in a dialog or a drawer, whose opening moved the focus inside.
    // Esc, as a dialog or a focused banner takes it; a removable badge is focused and removed with Delete.
    void dismiss(ui::Widget& widget) override {
        if (dynamic_cast<morph::tui::detail::BadgeImpl*>(&WidgetBase::of(widget)) != nullptr) {
            focusByClick(widget);
            settle();
            send(special(KeyCode::Delete));
            settle();
            return;
        }
        if (dynamic_cast<morph::tui::detail::BannerImpl*>(&WidgetBase::of(widget)) != nullptr) {
            focusByClick(widget);
        }
        settle();
        send(special(KeyCode::Escape));
        settle();
    }

    // With the keyboard, as a click toggles a row only once a half second has passed: the cursor to each row, and
    // Space. The rows not asked for are toggled off first, then the asked-for ones on, in the order given.
    void selectRows(ui::Widget& table, std::vector<std::size_t> const& rows) override {
        settle();
        auto& impl = tableOf(table);
        if (!focus(table)) {
            return;
        }
        auto const marked = impl.markedRows();
        auto const toggle = [this](std::size_t row) {
            send(special(KeyCode::Home));
            for (std::size_t step = 0; step < row; ++step) {
                send(special(KeyCode::Down));
            }
            send(key(core::tui::KeyEvent{.key = core::tui::keyCodeFromCodepoint(U' '), .codepoint = U' '}));
            settle();
        };
        bool const single = rows.size() == 1;
        for (std::size_t const row : marked) {
            if (!single && std::ranges::find(rows, row) == rows.end()) {
                toggle(row);
            }
        }
        for (std::size_t const row : rows) {
            auto const now = impl.markedRows();
            if (single || std::ranges::find(now, row) == now.end()) {
                toggle(row);
            }
        }
    }
    // A mouse click on the row's drawn cell, with Ctrl or Shift held as the terminal reports it in SGR mouse mode.
    void clickRow(ui::Widget& table, std::size_t row, ui::RowClick click) override {
        settle();
        ui::Widget const* const rowWidget = childAt(dynamic_cast<ui::ContainerWidget&>(table), row);
        auto const cell = rowWidget == nullptr ? std::nullopt : drawnCell(*rowWidget);
        if (!cell || _fault == Fault::DeadInput) {
            return;
        }
        Modifier modifiers = Modifier::None;
        if (click == ui::RowClick::Toggle) {
            modifiers = Modifier::Ctrl;
        } else if (click == ui::RowClick::Range) {
            modifiers = Modifier::Shift;
        }
        send(core::tui::MouseEvent{
            .type = Type::Press, .button = 0, .x = cell->x + 1, .y = cell->y + 1, .modifiers = modifiers});
        send(core::tui::MouseEvent{
            .type = Type::Release, .button = 0, .x = cell->x + 1, .y = cell->y + 1, .modifiers = modifiers});
        settle();
    }
    // A mouse click on the header line, over the column's first cell: the header sits on the line above the rows. A
    // Shift-click holds Shift as the terminal reports it in SGR mouse mode.
    void clickHeader(ui::Widget& table, std::size_t column, ui::HeaderClick click) override {
        settle();
        auto const* const container = dynamic_cast<ui::ContainerWidget*>(&table);
        ui::Widget const* const firstRow = container == nullptr ? nullptr : childAt(*container, 0);
        auto const* const rowContainer = dynamic_cast<ui::ContainerWidget const*>(firstRow);
        ui::Widget const* const cell = rowContainer == nullptr ? nullptr : childAt(*rowContainer, column);
        auto const at = cell == nullptr ? std::nullopt : drawnCell(*cell);
        auto const top = drawnCell(table);
        if (!at || !top || _fault == Fault::DeadInput) {
            return;
        }
        Modifier const modifiers = click == ui::HeaderClick::Shift ? Modifier::Shift : Modifier::None;
        send(core::tui::MouseEvent{
            .type = Type::Press, .button = 0, .x = at->x + 1, .y = top->y + 1, .modifiers = modifiers});
        send(core::tui::MouseEvent{
            .type = Type::Release, .button = 0, .x = at->x + 1, .y = top->y + 1, .modifiers = modifiers});
        settle();
    }

    // Shift+F10 on the focused header, or a click on the header line's `⋯` for the table's menu; the open list's
    // entries are read, and Esc closes it.
    [[nodiscard]] std::vector<ui::MenuEntry> headerMenu(ui::Widget& table,
                                                        std::optional<std::size_t> column) override {
        if (!openMenu(table, column)) {
            return {};
        }
        std::vector<ui::MenuEntry> entries = tableOf(table).openMenuEntries();
        send(special(KeyCode::Escape));
        settle();
        return entries;
    }
    // The menu opened as `headerMenu` opens it, then a click on each level's row.
    void chooseHeaderMenuEntry(ui::Widget& table, std::optional<std::size_t> column,
                               std::vector<std::size_t> const& path) override {
        if (!openMenu(table, column)) {
            return;
        }
        for (std::size_t const index : path) {
            auto const* const popup = tableOf(table).openPopup();
            if (popup == nullptr) {
                return;
            }
            auto const area = popup->screenBounds();
            int const row = area.y + static_cast<int>(index);
            send(core::tui::MouseEvent{.type = Type::Press, .button = 0, .x = area.x + 1, .y = row + 1});
            send(core::tui::MouseEvent{.type = Type::Release, .button = 0, .x = area.x + 1, .y = row + 1});
            settle();
        }
    }
    // Ctrl+Shift+Left or Right on the focused header, once per place, while the column moves.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): overrides the conformance suite's `moveColumn`, which documents (`column`, `position`)
    void moveColumn(ui::Widget& table, std::size_t column, std::size_t position) override {
        if (!focusHeader(table, column)) {
            return;
        }
        auto const& impl = tableOf(table);
        for (std::size_t step = 0; step < impl.visibleOrder().size(); ++step) {
            std::size_t const place = placeOf(impl, column);
            if (place == position) {
                return;
            }
            send(special(place < position ? KeyCode::Right : KeyCode::Left, Modifier::Ctrl | Modifier::Shift));
            settle();
            if (placeOf(impl, column) == place) {
                return;  // the move is reported; the mount has not rearranged the columns
            }
        }
    }
    // The header as the last frame drew it: the cells of the column's label on the header line, trimmed.
    [[nodiscard]] std::string headerText(ui::Widget const& table, std::size_t column) override {
        settle();
        if (_fault == Fault::WrongText) {
            return "?";
        }
        auto const area = tableOf(table).headerLabelArea(column);
        auto const bounds = WidgetBase::of(table).view().screenBounds();
        std::vector<std::string> const rows = morph::tui::testing::rowsOf(_screen);
        int const line = bounds.y + (area ? area->y : 0);
        if (!area || line < 0 || std::cmp_greater_equal(line, rows.size())) {
            return {};
        }
        std::u32string const cells =
            unicode::convert_to<char32_t>(std::string_view{rows.at(static_cast<std::size_t>(line))});
        auto const from = static_cast<std::size_t>(std::max(0, bounds.x + area->x));
        std::u32string label =
            from < cells.size() ? cells.substr(from, static_cast<std::size_t>(area->width)) : std::u32string{};
        while (!label.empty() && label.back() == U' ') {
            label.pop_back();
        }
        while (!label.empty() && label.front() == U' ') {
            label.erase(0, 1);
        }
        return unicode::convert_to<char>(std::u32string_view{label});
    }
    // A click on the filter button, the last cell of the column's header.
    void pressFilter(ui::Widget& table, std::size_t column) override {
        settle();
        auto const area = tableOf(table).headerLabelArea(column);
        auto const bounds = WidgetBase::of(table).view().screenBounds();
        if (area) {
            clickCell(bounds.x + area->x + area->width + 1, bounds.y + area->y);
        }
    }
    // A click on the group's filter button, the last cell of its span on the group line.
    void pressGroupFilter(ui::Widget& table, std::size_t group) override {
        settle();
        auto const x = tableOf(table).groupFilterX(group);
        auto const bounds = WidgetBase::of(table).view().screenBounds();
        if (x) {
            clickCell(bounds.x + *x, bounds.y);
        }
    }
    [[nodiscard]] std::string headerName(ui::Widget const& table, std::size_t column) override {
        return _fault == Fault::WrongText ? "?" : tableOf(table).headerName(column);
    }
    [[nodiscard]] std::string headerDescription(ui::Widget const& table, std::size_t column) override {
        return tableOf(table).headerDescription(column);
    }
    // What the OSC 52 sequence carried, as the mock terminal decoded it.
    [[nodiscard]] std::string clipboardText() override {
        auto const* const output = dynamic_cast<core::tui::MockTerminalOutput const*>(&_terminal.output());
        return output == nullptr ? std::string{} : output->clipboardText();
    }
    [[nodiscard]] std::vector<std::size_t> selectedRows(ui::Widget const& table) override {
        return tableOf(table).markedRows();
    }

    // Ctrl+Home, then Right once per character before the position.
    void moveCursor(ui::Widget& field, std::size_t position) override {
        focusByClick(field);
        send(special(KeyCode::Home, Modifier::Ctrl));
        auto const text = WidgetBase::of(field).probeText();
        auto const characters = unicode::convert_to<char32_t>(std::string_view{text}.substr(0, position)).size();
        for (std::size_t step = 0; step < characters; ++step) {
            send(special(KeyCode::Right));
        }
        settle();
    }
    [[nodiscard]] std::size_t cursorOf(ui::Widget const& field) override {
        auto const* const input = dynamic_cast<morph::tui::detail::TextInputImpl const*>(&WidgetBase::of(field));
        return input != nullptr ? input->cursor() : 0;
    }

    void press(ui::Widget& focused, std::string_view chord) override {
        settle();
        static_cast<void>(focus(focused));
        if (auto const pressed = morph::tui::detail::chordKey(chord)) {
            send(key(*pressed));
        }
        settle();
    }
    [[nodiscard]] bool hasFocus(ui::Widget const& widget) override {
        return _screen.focusedComponent() == &WidgetBase::of(widget).view();
    }

    // Backspace back to the top level, then a click on each level's row: a submenu's opens it, a leaf's chooses it.
    void chooseMenuEntry(ui::Widget& menu, std::vector<std::size_t> const& path) override {
        settle();
        // A button-style menu: a click on the trigger opens the list, from its top, and each index is a click on that
        // row of the list. A list left open by an earlier path is closed first.
        if (auto const* const button =
                dynamic_cast<morph::tui::detail::MenuButtonImpl const*>(&WidgetBase::of(menu))) {
            if (button->isOpen()) {
                send(special(KeyCode::Escape));
                settle();
            }
            click(menu);
            for (std::size_t const index : path) {
                auto const* const popup = button->openPopup();
                if (popup == nullptr) {
                    return;
                }
                auto const area = popup->screenBounds();
                int const row = area.y + static_cast<int>(index);
                send(core::tui::MouseEvent{.type = Type::Press, .button = 0, .x = area.x + 1, .y = row + 1});
                send(core::tui::MouseEvent{.type = Type::Release, .button = 0, .x = area.x + 1, .y = row + 1});
                settle();
            }
            return;
        }
        auto const* const impl = dynamic_cast<morph::tui::detail::MenuImpl const*>(&WidgetBase::of(menu));
        if (impl != nullptr && !impl->level().empty() && focus(menu)) {
            for (std::size_t depth = impl->level().size(); depth > 0; --depth) {
                send(special(KeyCode::Backspace));
            }
            settle();
        }
        for (std::size_t const index : path) {
            auto const cell = drawnCell(menu);
            if (!cell) {
                return;
            }
            int const row = cell->y + static_cast<int>(index);
            send(core::tui::MouseEvent{.type = Type::Press, .button = 0, .x = cell->x + 1, .y = row + 1});
            send(core::tui::MouseEvent{.type = Type::Release, .button = 0, .x = cell->x + 1, .y = row + 1});
            settle();
        }
    }

    // A click on the title when the section is not already as asked.
    void expand(ui::Widget& section, bool open) override {
        settle();
        auto const* const collapsible =
            dynamic_cast<morph::tui::detail::CollapsibleImpl const*>(&WidgetBase::of(section));
        if (collapsible != nullptr && collapsible->isOpen() == open) {
            return;
        }
        click(section);
    }

    // What a terminal does with files dropped on it: their paths, quoted, pasted to the focused widget.
    void dropFiles(ui::Widget& zone, std::vector<std::string> const& paths) override {
        focusByClick(zone);
        std::string pasted;
        for (auto const& path : paths) {
            pasted += (pasted.empty() ? "'" : " '") + path + "'";
        }
        send(core::tui::PasteEvent{.text = pasted});
        settle();
    }

    // In cells, where the widget was last drawn on the screen.
    std::optional<ui::testing::WidgetBounds> boundsOf(ui::Widget const& widget) override {
        settle();
        auto const bounds = WidgetBase::of(widget).view().screenBounds();
        return ui::testing::WidgetBounds{.x = static_cast<double>(bounds.x),
                                         .y = static_cast<double>(bounds.y),
                                         .width = static_cast<double>(bounds.width),
                                         .height = static_cast<double>(bounds.height)};
    }

    // The widget the spotlight rings now, as the terminal widget reports it.
    ui::Widget const* targetOf(ui::SpotlightWidget const& spotlight) override {
        settle();
        auto const* const impl = dynamic_cast<morph::tui::detail::SpotlightImpl const*>(&spotlight);
        WidgetBase const* const ringed = impl == nullptr ? nullptr : impl->ringed();
        return ringed == nullptr ? nullptr : &ringed->asWidget();
    }

    // In cells: the box around the non-blank cells the widget drew, read from the last frame.
    std::optional<ui::testing::WidgetBounds> textExtentOf(ui::Widget const& widget) override {
        settle();
        auto const bounds = WidgetBase::of(widget).view().screenBounds();
        std::vector<std::string> const rows = morph::tui::testing::rowsOf(_screen);
        int left = bounds.x + bounds.width;
        int right = bounds.x;
        for (int row = bounds.y; row < bounds.y + bounds.height && std::cmp_less(row, rows.size()); ++row) {
            std::string const& line = rows.at(static_cast<std::size_t>(row));
            for (int column = bounds.x; column < bounds.x + bounds.width && std::cmp_less(column, line.size());
                 ++column) {
                if (line.at(static_cast<std::size_t>(column)) != ' ') {
                    left = std::min(left, column);
                    right = std::max(right, column + 1);
                }
            }
        }
        return ui::testing::WidgetBounds{.x = static_cast<double>(left),
                                         .y = static_cast<double>(bounds.y),
                                         .width = static_cast<double>(std::max(0, right - left)),
                                         .height = static_cast<double>(bounds.height)};
    }

private:
    static WidgetBase const& containerBase(ui::ContainerWidget const& container) {
        return WidgetBase::of(static_cast<ui::Widget const&>(container));
    }
    static morph::tui::detail::ContainerBase const& containerOf(ui::ContainerWidget const& container) {
        return morph::tui::detail::ContainerBase::of(container);
    }
    static morph::tui::detail::TableImpl& tableOf(ui::Widget const& table) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): the probe drives the table it reads.
        return dynamic_cast<morph::tui::detail::TableImpl&>(const_cast<WidgetBase&>(WidgetBase::of(table)));
    }

    // The top left cell the widget was last drawn at, in the viewport; none when it has no drawn bounds, as a widget
    // never shown has none: a user cannot point at it.
    [[nodiscard]] std::optional<core::tui::Point> drawnCell(ui::Widget const& widget) const {
        auto const& view = WidgetBase::of(widget).view();
        auto const bounds = view.screenBounds();
        if (bounds.width <= 0 || bounds.height <= 0) {
            return std::nullopt;
        }
        return core::tui::Point{.x = bounds.x, .y = bounds.y};
    }

    // The focus moves to the widget as Tab would take it there: only a widget that can take it does.
    bool focus(ui::Widget const& widget) {
        auto& view = WidgetBase::of(widget).view();
        if (_screen.focusedComponent() == &view) {
            return true;
        }
        if (!view.focusable() || !view.visible()) {
            return false;
        }
        core::async::ExecutorScope const scope{_owner.coreExecutor()};
        _screen.setFocus(&view);
        return _screen.focusedComponent() == &view;
    }

    // The visible place of @p column in the table.
    static std::size_t placeOf(morph::tui::detail::TableImpl const& table, std::size_t column) {
        auto const order = table.visibleOrder();
        return static_cast<std::size_t>(std::distance(order.begin(), std::ranges::find(order, column)));
    }

    // The focus on the column's header, as a user's keys take it there: Up from the rows to the header, then Left to
    // the first header and Right to the column.
    bool focusHeader(ui::Widget& table, std::size_t column) {
        settle();
        if (!focus(table) || _fault == Fault::DeadInput) {
            return false;
        }
        auto const& impl = tableOf(table);
        for (std::size_t step = 0; step <= impl.children().size() && !impl.focusedHeader(); ++step) {
            send(special(KeyCode::Up));
        }
        auto const order = impl.visibleOrder();
        for (std::size_t step = 0; step < order.size(); ++step) {
            send(special(KeyCode::Left));
        }
        for (std::size_t step = 0; step < order.size() && impl.focusedHeader() != column; ++step) {
            send(special(KeyCode::Right));
        }
        settle();
        return impl.focusedHeader() == column;
    }

    // The column's menu by Shift+F10 on its header, or the table's by a click on the header line's last cell.
    bool openMenu(ui::Widget& table, std::optional<std::size_t> column) {
        if (column) {
            if (!focusHeader(table, *column)) {
                return false;
            }
            send(special(KeyCode::F10, Modifier::Shift));
        } else {
            settle();
            auto const bounds = WidgetBase::of(table).view().screenBounds();
            if (bounds.width <= 0 || _fault == Fault::DeadInput) {
                return false;
            }
            int const x = bounds.x + bounds.width - 1;
            send(core::tui::MouseEvent{.type = Type::Press, .button = 0, .x = x + 1, .y = bounds.y + 1});
            send(core::tui::MouseEvent{.type = Type::Release, .button = 0, .x = x + 1, .y = bounds.y + 1});
        }
        settle();
        return tableOf(table).openPopup() != nullptr;
    }

    // A left click on a viewport cell, 0-based.
    void clickCell(int x, int y) {
        send(core::tui::MouseEvent{.type = Type::Press, .button = 0, .x = x + 1, .y = y + 1});
        send(core::tui::MouseEvent{.type = Type::Release, .button = 0, .x = x + 1, .y = y + 1});
        settle();
    }

    void focusByClick(ui::Widget& widget) {
        settle();
        if (focus(widget)) {
            return;
        }
        click(widget);
    }

    static core::tui::InputEvent key(core::tui::KeyEvent event) { return event; }
    static core::tui::InputEvent special(KeyCode code, Modifier modifiers = Modifier::None) {
        return core::tui::KeyEvent{.key = code, .modifiers = modifiers, .codepoint = 0};
    }

    // One dispatch per event, with the owner current, as the session dispatches.
    void send(core::tui::InputEvent const& event) {
        if (_fault == Fault::DeadInput) {
            return;
        }
        core::async::ExecutorScope const scope{_owner.coreExecutor()};
        static_cast<void>(_screen.dispatchEvent(event));
    }

    Fault _fault;
    core::tui::Terminal _terminal{std::make_unique<core::tui::MockTerminalOutput>(80, 30)};
    core::tui::Screen _screen{_terminal};
    morph::tui::Backend _backend{_screen};
    morph::testing::StepExecutor _owner;
    morph::reactive::Runtime _runtime{_owner};
};

std::set<std::string_view> failingCases(Fault fault) {
    std::set<std::string_view> failing;
    for (auto const& testCase : ui::testing::conformanceCases()) {
        TuiProbe probe{fault};
        if (testCase.run(probe).has_value()) {
            failing.insert(testCase.name);
        }
    }
    return failing;
}

}  // namespace

// Mutation: report every case as passed from the probe's own run (the fault cases below then fail).
TEST_CASE("tui conformance: the terminal backend passes every case", "[tui][conformance]") {
    for (auto const& testCase : ui::testing::conformanceCases()) {
        TuiProbe probe;
        std::optional<std::string> const failure = testCase.run(probe);
        INFO(std::string{testCase.name} + ": " + failure.value_or("passed"));
        CHECK_FALSE(failure.has_value());
    }
}

TEST_CASE("tui conformance: the terminal backend passes every layout case", "[tui][conformance]") {
    for (auto const& testCase : ui::testing::layoutConformanceCases()) {
        TuiProbe probe;
        std::optional<std::string> const failure = testCase.run(probe);
        INFO(std::string{testCase.name} + ": " + failure.value_or("passed"));
        CHECK_FALSE(failure.has_value());
    }
}

TEST_CASE("tui conformance: the terminal backend passes every spotlight case", "[tui][conformance][tours]") {
    for (auto const& testCase : ui::testing::spotlightConformanceCases()) {
        TuiProbe probe;
        std::optional<std::string> const failure = testCase.run(probe);
        INFO(std::string{testCase.name} + ": " + failure.value_or("passed"));
        CHECK_FALSE(failure.has_value());
    }
}

// A probe that delivers no input, or misreads every text, must fail the cases that check input and text: otherwise
// the passes above say nothing about the terminal backend. Mutation: make `TuiProbe::send` deliver nothing in every
// mode (the first case fails), or deliver in DeadInput mode (the cases below pass).
TEST_CASE("tui conformance: the suite flags a probe that drops input or misreads text", "[tui][conformance]") {
    std::set<std::string_view> const deadInput = failingCases(Fault::DeadInput);
    for (std::string_view const name :
         {"a click runs the button's action once",
          "typing reaches onChange and a value set by the application is not echoed",
          "a drag onto an accepting target delivers the key",
          "a Table reports a user's selection as exactly the existing rows selected",
          "a chord goes to the innermost widget that declares it",
          "a chord pressed inside a hidden container runs nothing",
          "a menu reports only an enabled entry without a submenu", "a dismissed banner reports it and stays shown",
          "a closed collapsible's content takes no input, its header does",
          "a drop zone takes a drop whole or refuses it whole"}) {
        INFO(name);
        CHECK(deadInput.contains(name));
    }
    std::set<std::string_view> const wrongText = failingCases(Fault::WrongText);
    for (std::string_view const name :
         {"a Text shows its constant text", "a bound Text updates once per batch and not for an equal write",
          "a text field shows what the document makes of an edit"}) {
        INFO(name);
        CHECK(wrongText.contains(name));
    }
}

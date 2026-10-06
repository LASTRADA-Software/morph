// SPDX-License-Identifier: Apache-2.0

#include "tui/list_widgets.hpp"

#include <algorithm>
#include <core/tui/KeyCode.hpp>
#include <core/tui/Modifier.hpp>
#include <core/tui/Screen.hpp>
#include <core/tui/Theme.hpp>
#include <iterator>
#include <utility>
#include <variant>

#include "tui/drag.hpp"

namespace morph::tui::detail {

using ::core::tui::EventResult;
using ::core::tui::KeyCode;

namespace {

bool isPlain(::core::tui::KeyEvent const& key) noexcept {
    return ::core::tui::withoutLockKeys(key.modifiers) == ::core::tui::Modifier::None;
}

/// Whether @p key is the special key @p code, not a typed character whose codepoint has the same value.
bool isKey(::core::tui::KeyEvent const& key, KeyCode code) noexcept { return key.codepoint == 0 && key.key == code; }

std::optional<std::size_t> indexOf(std::vector<ui::SelectOption> const& options, std::optional<ui::Key> const& key) {
    if (!key) {
        return std::nullopt;
    }
    auto const found =
        std::ranges::find_if(options, [&key](ui::SelectOption const& option) { return option.key == *key; });
    if (found == options.end()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(std::distance(options.begin(), found));
}

std::string labelOf(std::vector<ui::SelectOption> const& options, std::optional<ui::Key> const& key) {
    auto const index = indexOf(options, key);
    return index ? options.at(*index).label : std::string{};
}

int widestLabel(std::vector<ui::SelectOption> const& options) {
    int widest = 0;
    for (auto const& option : options) {
        widest = std::max(widest, displayWidth(option.label));
    }
    return widest;
}

/// Replaces the rows of @p list, keeping the highlight on the row it was on, or on the last row when that is gone:
/// core::tui::List::setItems puts it back on the first.
void replaceRows(::core::tui::List& list, std::vector<::core::tui::ListItem> rows) {
    auto const highlight = list.selectedIndex();
    auto const count = rows.size();
    list.setItems(std::move(rows));
    if (count > 0) {
        list.setSelectedIndex(std::min(highlight, count - 1));
    }
}

/// The part of a list a widget shows: `rows` rows, from item `top` on.
struct RowWindow {
    std::size_t top = 0;  ///< The first item shown.
    int rows = 0;         ///< How many rows there are.
};

/// The first item of @p list to show in @p window's rows, as near its first item as keeps the highlighted one in view.
std::size_t scrolledTop(::core::tui::List const& list, RowWindow window) {
    auto const count = list.size();
    auto const highlight = list.selectedIndex();
    auto const shown = static_cast<std::size_t>(std::max(window.rows, 1));
    if (count <= shown) {
        return 0;
    }
    auto const first = std::min(window.top, count - shown);
    if (highlight < first) {
        return highlight;
    }
    if (highlight >= first + shown) {
        return highlight - shown + 1;
    }
    return first;
}

/// How `paintRows` draws a list.
struct RowsLook {
    bool reachable = true;  ///< Whether the user can reach the list; every row is muted when not.
    bool focused = false;   ///< Whether the list has the focus, which shows its highlighted row as selected.
};

::core::tui::Style const& rowStyle(::core::tui::Theme const& theme, RowsLook look, bool highlighted) {
    if (!look.reachable) {
        return theme.listItemDisabled;
    }
    return highlighted && look.focused ? theme.listItemSelected : theme.listItem;
}

/// Draws the items of @p list from item @p top on, one per row, `▶ ` before the highlighted item and two blanks before
/// the others. core::tui::List draws rows too, but it shortens a label by its bytes, which cuts off a label that
/// would fit and can cut a character in two.
void paintRows(::core::tui::List const& list, std::size_t top, RowsLook look, ::core::tui::Canvas& canvas) {
    auto const& theme = canvas.theme();
    auto const& items = list.items();
    auto const highlight = list.selectedIndex();
    for (int row = 0; row < canvas.height(); ++row) {
        auto const index = top + static_cast<std::size_t>(row);
        if (index >= items.size()) {
            return;
        }
        bool const highlighted = index == highlight;
        auto const& style = rowStyle(theme, look, highlighted);
        canvas.fill({.x = 0, .y = row, .width = canvas.width(), .height = 1}, ' ', style);
        int const marker = canvas.putString(row, 0, highlighted ? "▶ " : "  ", style);
        canvas.putString(row, marker, items.at(index).label, style);
    }
}

/// The plain list view (see `makeListView`).
class ListView final : public Hosted<::core::tui::List> {
public:
    ListView(WidgetBase& owner, ListOwner& lists) : Hosted{owner}, _lists{&lists} {}

    /// A list view has no children, so unlike a root View it has no drawn bounds below it to forget.
    void render(::core::tui::Canvas& canvas) override { owner().render(canvas); }

    /// A key the owner takes may end in a handler that destroys the owner and this view with it, so nothing is
    /// touched after that call.
    [[nodiscard]] EventResult onEvent(::core::tui::InputEvent const& event) override {
        if (escapeEndsDrag(owner().context(), event)) {
            return EventResult::Handled;
        }
        if (auto const* mouse = std::get_if<::core::tui::MouseEvent>(&event)) {
            return owner().pointer(*mouse);
        }
        if (!owner().actionable()) {
            return EventResult::Ignored;
        }
        if (auto const* key = std::get_if<::core::tui::KeyEvent>(&event)) {
            if (isKey(*key, KeyCode::Tab) || isKey(*key, KeyCode::Escape)) {
                return EventResult::Ignored;
            }
            if (auto const handled = _lists->listKey(*key)) {
                return *handled;
            }
        }
        return List::onEvent(event);
    }

private:
    ListOwner* _lists;
};

}  // namespace

std::unique_ptr<::core::tui::List> makeListView(WidgetBase& owner, ListOwner& lists) {
    return std::make_unique<ListView>(owner, lists);
}

void RadioSelectImpl::setOptions(std::vector<ui::SelectOption> const& options) {
    _options = options;
    rebuild();
}

void RadioSelectImpl::setSelected(std::optional<ui::Key> const& selected) {
    _selected = selected;
    rebuild();
    if (auto const index = indexOf(_options, _selected)) {
        _list->setSelectedIndex(*index);
    }
}

void RadioSelectImpl::setOnSelect(std::function<void(ui::Key)> onSelect) { _onSelect = std::move(onSelect); }

::core::tui::Size RadioSelectImpl::naturalSize() const {
    // "▶ " or "  ", then "(•) " or "( ) ", then the label.
    return {.width = widestLabel(_options) + 6, .height = std::max(1, static_cast<int>(_options.size()))};
}

std::string RadioSelectImpl::probeText() const { return labelOf(_options, _selected); }

void RadioSelectImpl::paint(::core::tui::Canvas& canvas) {
    _top = scrolledTop(*_list, {.top = _top, .rows = canvas.height()});
    paintRows(*_list, _top, {.reachable = actionable(), .focused = hasFocus()}, canvas);
}

void RadioSelectImpl::activate() { choose(_list->selectedIndex()); }

void RadioSelectImpl::click(::core::tui::Point cell) {
    if (cell.y < 0) {
        return;
    }
    auto const index = _top + static_cast<std::size_t>(cell.y);
    if (index < _options.size()) {
        _list->setSelectedIndex(index);
        choose(index);
    }
}

std::optional<EventResult> RadioSelectImpl::listKey(::core::tui::KeyEvent const& key) {
    if (!isActivation(key)) {
        return std::nullopt;
    }
    activate();
    return EventResult::Handled;
}

void RadioSelectImpl::rebuild() {
    std::vector<::core::tui::ListItem> rows;
    rows.reserve(_options.size());
    for (auto const& option : _options) {
        rows.push_back(::core::tui::ListItem{.label = (_selected == option.key ? "(•) " : "( ) ") + option.label});
    }
    replaceRows(*_list, std::move(rows));
    refresh();
}

// Optimistic, like the other choosers: the option is marked at once, and a binding that disagrees sets it back.
void RadioSelectImpl::choose(std::size_t index) {
    if (index >= _options.size() || !actionable()) {
        return;
    }
    auto picked = _options.at(index).key;
    _selected = picked;
    rebuild();
    auto const handler = _onSelect;
    if (handler) {
        handler(std::move(picked));
    }
}

/// The dropdown's option list, shown as an overlay. It has no parent, so the keys it leaves go no further.
class DropdownSelectImpl::Popup final : public ::core::tui::List {
public:
    explicit Popup(DropdownSelectImpl& owner) : _owner{&owner} {}

    void render(::core::tui::Canvas& canvas) override {
        _owner->_popupTop = scrolledTop(*this, {.top = _owner->_popupTop, .rows = canvas.height()});
        paintRows(*this, _owner->_popupTop, {.reachable = true, .focused = true}, canvas);
    }

    [[nodiscard]] ::core::tui::Size preferredSize() const override { return _owner->popupSize(); }

    /// Not drawn, and not hit by the pointer, while the list cannot be used. Closing it here would change the overlay
    /// list the screen is drawing from, so it closes at the next input that reaches it.
    [[nodiscard]] bool visible() const noexcept override { return List::visible() && _owner->listUsable(); }

    /// Choosing may end in a handler that destroys the owner and this list with it, so nothing is touched after it.
    [[nodiscard]] EventResult onEvent(::core::tui::InputEvent const& event) override {
        auto* const owner = _owner;
        if (escapeEndsDrag(owner->context(), event)) {
            return EventResult::Handled;
        }
        if (!owner->listUsable()) {
            owner->close();
            return EventResult::Ignored;
        }
        if (auto const* mouse = std::get_if<::core::tui::MouseEvent>(&event)) {
            return pointer(*mouse);
        }
        if (auto const* key = std::get_if<::core::tui::KeyEvent>(&event)) {
            if (isActivation(*key)) {
                owner->choose(selectedIndex());
                return EventResult::Handled;
            }
            if (isKey(*key, KeyCode::Escape)) {
                owner->close();
                return EventResult::Handled;
            }
            if (isKey(*key, KeyCode::Tab)) {
                owner->close();
                return EventResult::Ignored;
            }
        }
        return List::onEvent(event);
    }

    /// The focus left for another place, which took it: the list closes and leaves the focus there.
    void onBlur() override { _owner->close(); }

private:
    // A press here ends a gesture elsewhere whose release never arrived; the release of a left press that started
    // here chooses the option under it.
    [[nodiscard]] EventResult pointer(::core::tui::MouseEvent const& mouse) {
        using Type = ::core::tui::MouseEvent::Type;
        if (mouse.type == Type::ScrollUp || mouse.type == Type::ScrollDown) {
            if (mouse.type == Type::ScrollUp) {
                selectPrevious();
            } else {
                selectNext();
            }
            invalidate();
            return EventResult::Handled;
        }
        if (mouse.type == Type::Press) {
            _owner->context().endPress();
            _pressed = mouse.button == 0;
            return EventResult::Handled;
        }
        if (mouse.type != Type::Release || !std::exchange(_pressed, false)) {
            return EventResult::Handled;
        }
        auto const bounds = screenBounds();
        ::core::tui::Point const cell{.x = mouse.x - 1, .y = mouse.y - 1};
        if (cell.x >= 0 && cell.y >= 0 && cell.x < bounds.width && cell.y < bounds.height) {
            _owner->choose(_owner->_popupTop + static_cast<std::size_t>(cell.y));
        }
        return EventResult::Handled;
    }

    DropdownSelectImpl* _owner;
    bool _pressed = false;
};

std::unique_ptr<::core::tui::List> DropdownSelectImpl::makePopup() { return std::make_unique<Popup>(*this); }

DropdownSelectImpl::~DropdownSelectImpl() {
    if (!_open) {
        return;
    }
    _open = false;
    std::erase(context().popups, this);
    if (context().screen->focusedComponent() == _popup.get()) {
        static_cast<void>(focusWidget(context(), nullptr));
    }
    context().screen->hideOverlay(*_popup);
}

void DropdownSelectImpl::setOptions(std::vector<ui::SelectOption> const& options) {
    _options = options;
    if (_open && _options.empty()) {
        close();
    } else if (_open) {
        fillPopup();
    }
    refresh();
}

void DropdownSelectImpl::setSelected(std::optional<ui::Key> const& selected) {
    _selected = selected;
    if (auto const index = indexOf(_options, _selected); index && _open) {
        _popup->setSelectedIndex(*index);
    }
    refresh();
}

void DropdownSelectImpl::setOnSelect(std::function<void(ui::Key)> onSelect) { _onSelect = std::move(onSelect); }

::core::tui::Size DropdownSelectImpl::naturalSize() const { return {.width = widestLabel(_options) + 4, .height = 1}; }

std::string DropdownSelectImpl::probeText() const { return labelOf(_options, _selected); }

// The list follows the field when it moves (a scroll, a resized terminal): the field is drawn before the overlays.
void DropdownSelectImpl::paint(::core::tui::Canvas& canvas) {
    auto const& theme = canvas.theme();
    auto const* style = &theme.buttonDisabled;
    if (actionable()) {
        style = hasFocus() || _open ? &theme.buttonFocused : &theme.buttonNormal;
    }
    canvas.putString(0, 0, "[" + labelOf(_options, _selected) + " ▾]", *style);
    if (_open) {
        placePopup();
    }
}

EventResult DropdownSelectImpl::key(::core::tui::KeyEvent const& key) {
    if (isActivation(key) || (isPlain(key) && isKey(key, KeyCode::Down))) {
        return open() ? EventResult::Handled : EventResult::Ignored;
    }
    return EventResult::Ignored;
}

void DropdownSelectImpl::activate() { static_cast<void>(open()); }

void DropdownSelectImpl::closePopup() { close(); }

// The list takes the focus last: the focus leaving its old place may run a handler (a field committing what was
// typed), which may destroy this select, or close the list by putting the select out of reach.
bool DropdownSelectImpl::open() {
    if (_open || _options.empty() || !actionable() || !fieldDrawn()) {
        return false;
    }
    auto const place = placement();
    if (place.rows == 0) {
        return false;
    }
    fillPopup();
    _popupTop = 0;
    _popupRows = place.rows;
    _popupAt = place.origin;
    _open = true;
    context().popups.push_back(this);
    context().screen->showOverlay(*_popup, place.origin);
    refresh();
    return focusPopup(context(), *this, *_popup);
}

// Nothing here runs a handler: the focus going back to the field leaves only the list, whose blur ends here at once.
void DropdownSelectImpl::close() {
    if (!_open) {
        return;
    }
    _open = false;
    std::erase(context().popups, this);
    auto& screen = *context().screen;
    bool const hadFocus = screen.focusedComponent() == _popup.get();
    screen.hideOverlay(*_popup);
    refresh();
    if (hadFocus) {
        static_cast<void>(focusWidget(context(), view().screen() != nullptr ? this : nullptr));
    }
}

// Optimistic, like the other choosers: the field shows the choice at once.
void DropdownSelectImpl::choose(std::size_t index) {
    if (index >= _options.size() || !actionable()) {
        close();
        return;
    }
    auto picked = _options.at(index).key;
    _selected = picked;
    close();
    auto const handler = _onSelect;
    if (handler) {
        handler(std::move(picked));
    }
}

void DropdownSelectImpl::fillPopup() {
    std::vector<::core::tui::ListItem> rows;
    rows.reserve(_options.size());
    for (auto const& option : _options) {
        rows.push_back(::core::tui::ListItem{.label = option.label});
    }
    replaceRows(*_popup, std::move(rows));
    if (auto const index = indexOf(_options, _selected)) {
        _popup->setSelectedIndex(*index);
    }
}

DropdownSelectImpl::Placement DropdownSelectImpl::placement() const {
    auto const field = view().screenBounds();
    auto const viewport = context().screen->viewportArea();
    int const count = static_cast<int>(_options.size());
    // The field draws on its first row, whatever height it was laid out at.
    int const below = std::max(0, viewport.y + viewport.height - (field.y + 1));
    int const above = std::max(0, field.y - viewport.y);
    int const width = popupSize().width;
    Placement place{.origin = {.x = std::max(viewport.x, std::min(field.x, viewport.x + viewport.width - width)),
                               .y = field.y + 1},
                    .rows = std::min(count, below)};
    if (below < count && above > below) {
        place.rows = std::min(count, above);
        place.origin.y = field.y - place.rows;
    }
    return place;
}

void DropdownSelectImpl::placePopup() {
    auto& screen = *context().screen;
    auto const place = placement();
    _popupRows = place.rows;
    if (place.origin != _popupAt || !screen.isOverlayVisible(*_popup)) {
        _popupAt = place.origin;
        screen.showOverlay(*_popup, place.origin);
    }
}

// As wide as the field, or as the widest option and the marker before it.
::core::tui::Size DropdownSelectImpl::popupSize() const {
    return {.width = std::max(widestLabel(_options) + 2, view().screenBounds().width), .height = _popupRows};
}

bool DropdownSelectImpl::fieldDrawn() const { return view().screen() != nullptr && !view().screenBounds().empty(); }

bool DropdownSelectImpl::listUsable() const { return actionable() && fieldDrawn() && _popupRows > 0; }

void MenuImpl::setItems(std::vector<std::string> const& items) {
    _labels = items;
    std::vector<::core::tui::ListItem> rows;
    rows.reserve(_labels.size());
    for (auto const& label : _labels) {
        rows.push_back(::core::tui::ListItem{.label = label});
    }
    replaceRows(*_list, std::move(rows));
    refresh();
}

void MenuImpl::setOnActivate(std::function<void(std::size_t)> onActivate) { _onActivate = std::move(onActivate); }

::core::tui::Size MenuImpl::naturalSize() const {
    int widest = 0;
    for (auto const& label : _labels) {
        widest = std::max(widest, displayWidth(label));
    }
    return {.width = widest + 2, .height = std::max(1, static_cast<int>(_labels.size()))};
}

std::string MenuImpl::probeText() const {
    auto const index = _list->selectedIndex();
    return index < _labels.size() ? _labels.at(index) : std::string{};
}

void MenuImpl::paint(::core::tui::Canvas& canvas) {
    _top = scrolledTop(*_list, {.top = _top, .rows = canvas.height()});
    paintRows(*_list, _top, {.reachable = actionable(), .focused = hasFocus()}, canvas);
}

void MenuImpl::activate() {
    if (_labels.empty() || !actionable()) {
        return;
    }
    auto const index = _list->selectedIndex();
    auto const handler = _onActivate;
    if (handler) {
        handler(index);
    }
}

void MenuImpl::click(::core::tui::Point cell) {
    if (cell.y < 0) {
        return;
    }
    auto const index = _top + static_cast<std::size_t>(cell.y);
    if (index < _labels.size()) {
        _list->setSelectedIndex(index);
        activate();
    }
}

std::optional<EventResult> MenuImpl::listKey(::core::tui::KeyEvent const& key) {
    if (!isActivation(key)) {
        return std::nullopt;
    }
    activate();
    return EventResult::Handled;
}

void TabsImpl::setTabs(std::vector<std::string> const& tabs) {
    _labels = tabs;
    refresh();
}

// The highlight only: which page shows is the mount's, through each page slot's own visible flag. A child's
// position says nothing about its tab, because the pages arrive in the order their tabs were first shown.
void TabsImpl::setSelected(std::size_t index) {
    _selected = index;
    refresh();
}

void TabsImpl::setOnSelect(std::function<void(std::size_t)> onSelect) { _onSelect = std::move(onSelect); }

std::string TabsImpl::labelText(std::size_t index) const {
    auto const& label = _labels.at(index);
    return index == _selected ? "[" + label + "]" : " " + label + " ";
}

::core::tui::Size TabsImpl::naturalSize() const {
    int bar = 0;
    for (std::size_t index = 0; index < _labels.size(); ++index) {
        bar += displayWidth(labelText(index));
    }
    auto const body = stackNaturalSize(children(), ui::Axis::Vertical, 0);
    return {.width = std::max(bar, body.width), .height = 1 + body.height};
}

std::string TabsImpl::probeText() const { return _selected < _labels.size() ? _labels.at(_selected) : std::string{}; }

std::vector<::core::tui::Rect> TabsImpl::childAreas(::core::tui::Size size) const {
    return stackAreas(children(), {.x = 0, .y = 1, .width = size.width, .height = std::max(0, size.height - 1)},
                      StackSpec{.axis = ui::Axis::Vertical});
}

void TabsImpl::paint(::core::tui::Canvas& canvas) {
    auto const& theme = canvas.theme();
    auto const* marked = &theme.textMuted;
    if (actionable()) {
        marked = hasFocus() ? &theme.buttonFocused : &theme.textBold;
    }
    int column = 0;
    for (std::size_t index = 0; index < _labels.size(); ++index) {
        column += canvas.putString(0, column, labelText(index), index == _selected ? *marked : theme.textMuted);
    }
    placeChildren(childAreas(canvas.size()));
}

// A key a focused page widget ignored bubbles up to this view; only the tabs' own focus switches tabs.
EventResult TabsImpl::key(::core::tui::KeyEvent const& key) {
    if (!hasFocus() || !isPlain(key) || _labels.empty()) {
        return EventResult::Ignored;
    }
    bool const none = _selected >= _labels.size();
    if (isKey(key, KeyCode::Left)) {
        if (none) {
            select(_labels.size() - 1);
        } else if (_selected > 0) {
            select(_selected - 1);
        }
        return EventResult::Handled;
    }
    if (isKey(key, KeyCode::Right)) {
        if (none) {
            select(0);
        } else if (_selected + 1 < _labels.size()) {
            select(_selected + 1);
        }
        return EventResult::Handled;
    }
    return EventResult::Ignored;
}

// A press anywhere on the tabs that no page widget takes bubbles here, so only a tab's label focuses and selects.
// Taking the focus may run a handler (a field committing) that destroys the tabs.
void TabsImpl::click(::core::tui::Point cell) {
    if (cell.y != 0 || !focusable()) {
        return;
    }
    int right = 0;
    for (std::size_t index = 0; index < _labels.size(); ++index) {
        right += displayWidth(labelText(index));
        if (cell.x < right) {
            if (focusWidget(context(), this)) {
                select(index);
            }
            return;
        }
    }
}

// Optimistic, like the other choosers: the bar shows the choice at once, and the mount swaps the pages. The mount may
// set the highlight back from inside the handler, when the new page fails to mount; nothing here touches the tabs
// after the handler. It is reached only from `key`, which `dispatch` calls only while the tabs are actionable, and
// from `click`, after the release has checked that and `focusWidget` has refused tabs that are not.
void TabsImpl::select(std::size_t index) {
    if (index >= _labels.size() || index == _selected) {
        return;
    }
    _selected = index;
    refresh();
    auto const handler = _onSelect;
    if (handler) {
        handler(index);
    }
}

}  // namespace morph::tui::detail

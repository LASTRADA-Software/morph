// SPDX-License-Identifier: Apache-2.0

#include "tui/list_widgets.hpp"

#include <algorithm>
#include <core/tui/KeyCode.hpp>
#include <core/tui/Modifier.hpp>
#include <core/tui/Screen.hpp>
#include <core/tui/Theme.hpp>
#include <iterator>
#include <morph/ui/theme.hpp>
#include <optional>
#include <string>
#include <string_view>
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

/// The `filterText` of a menu's separator row, which `paintRows` draws as a line; no label can be it, since a label
/// is never filtered by it.
constexpr std::string_view kSeparatorRow = "\x1f-separator";

/// Draws the items of @p list from item @p top on, one per row, `▶ ` before the highlighted item and two blanks before
/// the others, and an item's description after its label, muted. A separator row is a line across the list.
/// core::tui::List draws rows too, but it shortens a label by its bytes, which cuts off a label that would fit and can
/// cut a character in two.
void paintRows(::core::tui::List const& list, std::size_t top, RowsLook look, ::core::tui::Canvas& canvas) {
    auto const& theme = canvas.theme();
    auto const& items = list.items();
    auto const highlight = list.selectedIndex();
    for (int row = 0; row < canvas.height(); ++row) {
        auto const index = top + static_cast<std::size_t>(row);
        if (index >= items.size()) {
            return;
        }
        auto const& item = items.at(index);
        if (item.filterText == kSeparatorRow) {
            for (int column = 0; column < canvas.width(); ++column) {
                canvas.put(row, column, "─", theme.listItemDisabled);
            }
            continue;
        }
        bool const highlighted = index == highlight;
        auto const& style = item.enabled ? rowStyle(theme, look, highlighted) : theme.listItemDisabled;
        canvas.fill({.x = 0, .y = row, .width = canvas.width(), .height = 1}, ' ', style);
        int const marker = canvas.putString(row, 0, highlighted ? "▶ " : "  ", style);
        int const labelEnd = marker + canvas.putString(row, marker, item.label, style);
        if (!item.description.empty()) {
            canvas.putString(row, labelEnd + 2, item.description, theme.textMuted);
        }
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
            // The widget's own chords come before the list's keys, so a chord it declares takes that key from it.
            if (EventResult const result = owner().chord(*key); result != EventResult::Ignored) {
                return result;
            }
            if (isKey(*key, KeyCode::Tab) || isKey(*key, KeyCode::Escape)) {
                return EventResult::Ignored;
            }
            if (auto const handled = _lists->listKey(*key)) {
                return *handled;
            }
            return List::onEvent(event);
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

::core::tui::Size RadioSelectImpl::controlSize() const {
    // "▶ " or "  ", then "(•) " or "( ) ", then the label.
    return withCaption(
        {.width = widestLabel(_options) + 6, .height = std::max(1, static_cast<int>(_options.size()))});
}

std::string RadioSelectImpl::probeText() const { return labelOf(_options, _selected); }

void RadioSelectImpl::paintControl(::core::tui::Canvas& canvas) {
    auto rows = paintCaption(canvas);
    _top = scrolledTop(*_list, {.top = _top, .rows = rows.height()});
    paintRows(*_list, _top, {.reachable = actionable(), .focused = hasFocus()}, rows);
    paintMarks(rows);
}

void RadioSelectImpl::activate() { choose(_list->selectedIndex()); }

void RadioSelectImpl::click(::core::tui::Point cell) {
    int const row = cell.y - captionRows();
    if (row < 0) {
        return;
    }
    auto const index = _top + static_cast<std::size_t>(row);
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
    if (index >= _options.size() || !actionable() || readOnly()) {
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

// ── Segmented ─────────────────────────────────────────────────────────────

namespace {

// A segment's text: its label padded by a space on each side.
std::string segmentText(ui::SelectOption const& option) { return " " + option.label + " "; }

}  // namespace

void SegmentedSelectImpl::setOptions(std::vector<ui::SelectOption> const& options) {
    _options = options;
    refresh();
}

void SegmentedSelectImpl::setSelected(std::optional<ui::Key> const& selected) {
    _selected = selected;
    refresh();
}

::core::tui::Size SegmentedSelectImpl::controlSize() const {
    int width = 0;
    for (ui::SelectOption const& option : _options) {
        width += displayWidth(segmentText(option)) + (width == 0 ? 0 : 1);
    }
    return withCaption({.width = width, .height = 1});
}

std::string SegmentedSelectImpl::probeText() const { return labelOf(_options, _selected); }

void SegmentedSelectImpl::paintControl(::core::tui::Canvas& canvas) {
    auto const& theme = canvas.theme();
    auto control = paintCaption(canvas);
    auto const marked = indexOf(_options, _selected);
    int column = 0;
    for (std::size_t index = 0; index < _options.size(); ++index) {
        if (index > 0) {
            column += control.putString(0, column, "│", theme.textMuted);
        }
        ::core::tui::Style style = theme.textNormal;
        if (!actionable()) {
            style = theme.textMuted;
        } else if (marked == index) {
            style = hasFocus() ? theme.buttonFocused : theme.listItemSelected;
        }
        column += control.putString(0, column, segmentText(_options.at(index)), style);
    }
    paintMarks(control);
}

EventResult SegmentedSelectImpl::key(::core::tui::KeyEvent const& key) {
    if (!isPlain(key) || (!isKey(key, KeyCode::Left) && !isKey(key, KeyCode::Right))) {
        return EventResult::Ignored;
    }
    auto const marked = indexOf(_options, _selected);
    if (!marked) {
        return EventResult::Ignored;
    }
    bool const left = isKey(key, KeyCode::Left);
    if ((left && *marked > 0) || (!left && *marked + 1 < _options.size())) {
        choose(left ? *marked - 1 : *marked + 1);
    }
    return EventResult::Handled;
}

void SegmentedSelectImpl::click(::core::tui::Point cell) {
    int const top = captionRows();
    if (cell.y != top) {
        return;
    }
    int column = 0;
    for (std::size_t index = 0; index < _options.size(); ++index) {
        int const width = displayWidth(segmentText(_options.at(index)));
        if (cell.x >= column && cell.x < column + width) {
            choose(index);
            return;
        }
        column += width + 1;
    }
}

// Optimistic, like the other choosers: the segment shows the choice at once.
void SegmentedSelectImpl::choose(std::size_t index) {
    if (index >= _options.size() || !actionable() || readOnly()) {
        return;
    }
    auto picked = _options.at(index).key;
    _selected = picked;
    refresh();
    auto const handler = _onSelect;
    if (handler) {
        handler(std::move(picked));
    }
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

void DropdownSelectImpl::setOnOpen(ui::Action onOpen) { _onOpen = std::move(onOpen); }

::core::tui::Size DropdownSelectImpl::controlSize() const {
    return withCaption({.width = widestLabel(_options) + 4, .height = 1});
}

std::string DropdownSelectImpl::probeText() const { return labelOf(_options, _selected); }

// The list follows the field when it moves (a scroll, a resized terminal): the field is drawn before the overlays.
void DropdownSelectImpl::paintControl(::core::tui::Canvas& canvas) {
    auto const& theme = canvas.theme();
    auto const* style = &theme.buttonDisabled;
    if (actionable()) {
        style = hasFocus() || _open ? &theme.buttonFocused : &theme.buttonNormal;
    }
    auto control = paintCaption(canvas);
    control.putString(0, 0, "[" + labelOf(_options, _selected) + " ▾]", *style);
    paintMarks(control);
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
// typed), which may destroy this select, or close the list by putting the select out of reach. `onOpen` runs last for
// the same reason, from a copy, and nothing of this select is touched after it.
bool DropdownSelectImpl::open() {
    if (_open || !actionable() || readOnly() || !fieldDrawn()) {
        return false;
    }
    if (_options.empty()) {
        // Nothing to show; the user still asked, and the handler may fetch what to show next time.
        ui::Action const onOpen = _onOpen;
        if (onOpen) {
            onOpen();
        }
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
    ui::Action const onOpen = _onOpen;
    bool const focused = focusPopup(context(), *this, *_popup);
    if (focused && onOpen) {
        onOpen();
    }
    return focused;
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
    if (index >= _options.size() || !actionable() || readOnly()) {
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
    // The field draws on its first row below its caption, whatever height it was laid out at.
    int const top = field.y + captionRows();
    int const below = std::max(0, viewport.y + viewport.height - (top + 1));
    int const above = std::max(0, top - viewport.y);
    int const width = popupSize().width;
    Placement place{
        .origin = {.x = std::max(viewport.x, std::min(field.x, viewport.x + viewport.width - width)), .y = top + 1},
        .rows = std::min(count, below)};
    if (below < count && above > below) {
        place.rows = std::min(count, above);
        place.origin.y = top - place.rows;
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

namespace {

std::string menuRowLabel(ui::MenuEntry const& entry) {
    std::string label;
    if (entry.checked.has_value()) {
        label += *entry.checked ? "[x] " : "[ ] ";
    }
    label += entry.label;
    if (!entry.items.empty()) {
        label += " >";
    }
    if (!entry.keys.empty()) {
        label += "  " + entry.keys;
    }
    return label;
}

// A row's label and, two cells after it, its description.
int menuRowWidth(ui::MenuEntry const& entry) {
    int const label = displayWidth(menuRowLabel(entry));
    return entry.description.empty() ? label : label + 2 + displayWidth(entry.description);
}

// NOLINTNEXTLINE(misc-no-recursion): one level per submenu, as deep as the document nests them.
int widestMenuRow(std::vector<ui::MenuEntry> const& entries) {
    int widest = 0;
    for (auto const& entry : entries) {
        widest = std::max({widest, menuRowWidth(entry), widestMenuRow(entry.items)});
    }
    return widest;
}

// The entries of the level @p path names; empty when it names none.
std::vector<ui::MenuEntry> const& menuLevel(std::vector<ui::MenuEntry> const& entries,
                                            std::vector<std::size_t> const& path) {
    static std::vector<ui::MenuEntry> const kNone;
    std::vector<ui::MenuEntry> const* level = &entries;
    for (std::size_t const index : path) {
        if (index >= level->size()) {
            return kNone;
        }
        level = &level->at(index).items;
    }
    return *level;
}

// Whether every entry on @p path is enabled.
bool menuPathEnabled(std::vector<ui::MenuEntry> const& entries, std::vector<std::size_t> const& path) {
    std::vector<ui::MenuEntry> const* level = &entries;
    for (std::size_t const index : path) {
        if (index >= level->size() || !level->at(index).enabled) {
            return false;
        }
        level = &level->at(index).items;
    }
    return true;
}

// The rows of one level: a separator a row the cursor skips, an entry disabled when it or the level is.
std::vector<::core::tui::ListItem> menuRows(std::vector<ui::MenuEntry> const& level, bool enabled) {
    std::vector<::core::tui::ListItem> rows;
    rows.reserve(level.size());
    for (auto const& entry : level) {
        if (entry.separator) {
            rows.push_back(::core::tui::ListItem{.filterText = std::string{kSeparatorRow}, .enabled = false});
        } else {
            rows.push_back(::core::tui::ListItem{.label = menuRowLabel(entry),
                                                 .description = entry.description,
                                                 .enabled = enabled && entry.enabled});
        }
    }
    return rows;
}

// The path to choose for the entry at @p index of the level @p path shows: none for a separator, an entry with a
// submenu, or an entry that is disabled or inside a disabled one.
std::optional<std::vector<std::size_t>> menuChoice(std::vector<ui::MenuEntry> const& entries,
                                                   std::vector<std::size_t> const& path, std::size_t index) {
    auto const& level = menuLevel(entries, path);
    if (index >= level.size() || level.at(index).separator || !level.at(index).items.empty() ||
        !level.at(index).enabled || !menuPathEnabled(entries, path)) {
        return std::nullopt;
    }
    auto chosen = path;
    chosen.push_back(index);
    return chosen;
}

}  // namespace

// The level shown is kept while it still exists in the new tree, so a binding that relabels an entry does not throw
// the user out of the submenu they are in.
void MenuImpl::setItems(std::vector<ui::MenuEntry> const& entries) {
    _entries = entries;
    std::vector<ui::MenuEntry> const* level = &_entries;
    std::size_t depth = 0;
    for (; depth < _path.size(); ++depth) {
        if (_path.at(depth) >= level->size() || level->at(_path.at(depth)).items.empty()) {
            break;
        }
        level = &level->at(_path.at(depth)).items;
    }
    _path.resize(depth);
    showLevel(_list->selectedIndex());
}

void MenuImpl::setTrigger(ui::MenuTrigger const& trigger) { static_cast<void>(trigger); }

void MenuImpl::setOnActivate(std::function<void(std::vector<std::size_t>)> onActivate) {
    _onActivate = std::move(onActivate);
}

std::vector<ui::MenuEntry> const& MenuImpl::shown() const { return menuLevel(_entries, _path); }

bool MenuImpl::levelEnabled() const { return menuPathEnabled(_entries, _path); }

void MenuImpl::showLevel(std::size_t highlighted) {
    std::vector<::core::tui::ListItem> rows = menuRows(shown(), levelEnabled());
    auto const count = rows.size();
    _list->setItems(std::move(rows));
    if (count > 0) {
        _list->setSelectedIndex(std::min(highlighted, count - 1));
    }
    _top = 0;
    refresh();
}

void MenuImpl::highlight(std::size_t index) {
    if (index < shown().size()) {
        _list->setSelectedIndex(index);
        refresh();
    }
}

void MenuImpl::enter(std::size_t index) {
    _path.push_back(index);
    showLevel(0);
}

void MenuImpl::leave() {
    if (_path.empty()) {
        return;
    }
    auto const from = _path.back();
    _path.pop_back();
    showLevel(from);
}

::core::tui::Size MenuImpl::naturalSize() const {
    std::size_t tallest = _entries.size();
    for (auto const& entry : _entries) {
        tallest = std::max(tallest, entry.items.size());
    }
    return {.width = widestMenuRow(_entries) + 2, .height = std::max(1, static_cast<int>(tallest))};
}

std::string MenuImpl::probeText() const {
    auto const index = _list->selectedIndex();
    return index < shown().size() ? shown().at(index).label : std::string{};
}

void MenuImpl::paint(::core::tui::Canvas& canvas) {
    _top = scrolledTop(*_list, {.top = _top, .rows = canvas.height()});
    paintRows(*_list, _top, {.reachable = actionable(), .focused = hasFocus()}, canvas);
}

// The handler gets the path, and may destroy this menu.
void MenuImpl::activate() {
    auto const index = _list->selectedIndex();
    if (index >= shown().size() || !actionable()) {
        return;
    }
    auto const& entry = shown().at(index);
    if (!entry.items.empty()) {
        enter(index);
        return;
    }
    auto choice = menuChoice(_entries, _path, index);
    if (!choice) {
        return;
    }
    auto path = std::move(*choice);
    // Choosing closes the submenus, as a menu does: the next choice starts from the top, at the entry it went
    // down from.
    if (!_path.empty()) {
        auto const from = _path.front();
        _path.clear();
        showLevel(from);
    }
    auto const handler = _onActivate;
    if (handler) {
        handler(std::move(path));
    }
}

void MenuImpl::click(::core::tui::Point cell) {
    if (cell.y < 0) {
        return;
    }
    auto const index = _top + static_cast<std::size_t>(cell.y);
    if (index < shown().size() && !shown().at(index).separator) {
        _list->setSelectedIndex(index);
        activate();
    }
}

std::optional<EventResult> MenuImpl::listKey(::core::tui::KeyEvent const& key) {
    if (isActivation(key)) {
        activate();
        return EventResult::Handled;
    }
    if (!isPlain(key)) {
        return std::nullopt;
    }
    if (isKey(key, KeyCode::Right)) {
        auto const index = _list->selectedIndex();
        if (index < shown().size() && !shown().at(index).items.empty()) {
            enter(index);
            return EventResult::Handled;
        }
        return std::nullopt;
    }
    if ((isKey(key, KeyCode::Left) || isKey(key, KeyCode::Backspace)) && !_path.empty()) {
        leave();
        return EventResult::Handled;
    }
    return std::nullopt;
}

/// The button-style menu's entry list, shown as an overlay. It has no parent, so the keys it leaves go no further.
class MenuButtonImpl::Popup final : public ::core::tui::List {
public:
    explicit Popup(MenuButtonImpl& owner) : _owner{&owner} {}

    void render(::core::tui::Canvas& canvas) override {
        _owner->_popupTop = scrolledTop(*this, {.top = _owner->_popupTop, .rows = canvas.height()});
        paintRows(*this, _owner->_popupTop, {.reachable = true, .focused = true}, canvas);
    }

    [[nodiscard]] ::core::tui::Size preferredSize() const override { return _owner->popupSize(); }

    /// Not drawn, and not hit by the pointer, while the list cannot be used; it closes at the next input that
    /// reaches it, as the dropdown select's does.
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
            if (auto const handled = levelKey(*key)) {
                return *handled;
            }
        }
        return List::onEvent(event);
    }

    /// The focus left for another place, which took it: the list closes and leaves the focus there.
    void onBlur() override { _owner->close(); }

private:
    [[nodiscard]] std::optional<EventResult> levelKey(::core::tui::KeyEvent const& key) {
        auto* const owner = _owner;
        if (isActivation(key)) {
            owner->pick(selectedIndex());
            return EventResult::Handled;
        }
        if (isKey(key, KeyCode::Escape)) {
            owner->close();
            return EventResult::Handled;
        }
        if (isKey(key, KeyCode::Tab)) {
            owner->close();
            return EventResult::Ignored;
        }
        if (!isPlain(key)) {
            return std::nullopt;
        }
        auto const& level = menuLevel(owner->_entries, owner->_path);
        if (isKey(key, KeyCode::Right) && selectedIndex() < level.size() && !level.at(selectedIndex()).items.empty()) {
            owner->enter(selectedIndex());
            return EventResult::Handled;
        }
        if ((isKey(key, KeyCode::Left) || isKey(key, KeyCode::Backspace)) && !owner->_path.empty()) {
            owner->leave();
            return EventResult::Handled;
        }
        return std::nullopt;
    }

    // A press here ends a gesture elsewhere whose release never arrived; the release of a left press that started
    // here picks the entry under it.
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
            _owner->pick(_owner->_popupTop + static_cast<std::size_t>(cell.y));
        }
        return EventResult::Handled;
    }

    MenuButtonImpl* _owner;
    bool _pressed = false;
};

std::unique_ptr<::core::tui::List> MenuButtonImpl::makePopup() { return std::make_unique<Popup>(*this); }

MenuButtonImpl::~MenuButtonImpl() {
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

// The level shown is kept while it still exists in the new tree, so a binding that relabels an entry does not throw
// the user out of the submenu they are in.
void MenuButtonImpl::setItems(std::vector<ui::MenuEntry> const& entries) {
    _entries = entries;
    std::size_t depth = 0;
    for (std::vector<ui::MenuEntry> const* level = &_entries; depth < _path.size(); ++depth) {
        if (_path.at(depth) >= level->size() || level->at(_path.at(depth)).items.empty()) {
            break;
        }
        level = &level->at(_path.at(depth)).items;
    }
    _path.resize(depth);
    if (_open && _entries.empty()) {
        close();
    } else if (_open) {
        fillPopup(_popup->selectedIndex());
    }
    refresh();
}

void MenuButtonImpl::setTrigger(ui::MenuTrigger const& trigger) {
    _trigger = trigger;
    refresh();
}

void MenuButtonImpl::setOnActivate(std::function<void(std::vector<std::size_t>)> onActivate) {
    _onActivate = std::move(onActivate);
}

// The label and a `▾`; an icon-only trigger its icon's glyph, else its accessible name, so it is never blank.
std::string MenuButtonImpl::triggerText() const {
    if (!_trigger.label.empty()) {
        return _trigger.label + " ▾";
    }
    std::string_view const glyph = ui::iconGlyph(_trigger.icon);
    if (!glyph.empty()) {
        return std::string{glyph};
    }
    return accessibleName().empty() ? std::string{"▾"} : accessibleName();
}

::core::tui::Size MenuButtonImpl::naturalSize() const {
    return {.width = displayWidth(triggerText()) + 2, .height = 1};
}

// The list follows the trigger when it moves: the trigger is drawn before the overlays.
void MenuButtonImpl::paint(::core::tui::Canvas& canvas) {
    auto const& theme = canvas.theme();
    ::core::tui::Style style = theme.buttonDisabled;
    if (actionable()) {
        style = hasFocus() || _open ? theme.buttonFocused : theme.buttonNormal;
        // As a push button: primary is bold, danger bold in the error colour, ghost plain.
        if (_trigger.emphasis == ui::ButtonEmphasis::Primary || _trigger.emphasis == ui::ButtonEmphasis::Danger) {
            style.bold = true;
        }
        if (_trigger.emphasis == ui::ButtonEmphasis::Danger) {
            style.fg = theme.error.fg;
        }
    }
    canvas.putString(0, 0, "[" + triggerText() + "]", style);
    if (_open) {
        placePopup();
    }
}

EventResult MenuButtonImpl::key(::core::tui::KeyEvent const& key) {
    if (isActivation(key) || (isPlain(key) && isKey(key, KeyCode::Down))) {
        return open() ? EventResult::Handled : EventResult::Ignored;
    }
    return EventResult::Ignored;
}

void MenuButtonImpl::activate() { static_cast<void>(open()); }

void MenuButtonImpl::closePopup() { close(); }

// The list takes the focus last: the focus leaving its old place may run a handler, which may destroy this menu.
bool MenuButtonImpl::open() {
    if (_open || !actionable() || !triggerDrawn() || _entries.empty()) {
        return false;
    }
    _path.clear();
    _popupTop = 0;
    auto const place = placement();
    if (place.rows == 0) {
        return false;
    }
    fillPopup(0);
    _popupRows = place.rows;
    _popupAt = place.origin;
    _open = true;
    context().popups.push_back(this);
    context().screen->showOverlay(*_popup, place.origin);
    refresh();
    return focusPopup(context(), *this, *_popup);
}

// Nothing here runs a handler: the focus going back to the trigger leaves only the list, whose blur ends here at once.
void MenuButtonImpl::close() {
    if (!_open) {
        return;
    }
    _open = false;
    _path.clear();
    std::erase(context().popups, this);
    auto& screen = *context().screen;
    bool const hadFocus = screen.focusedComponent() == _popup.get();
    screen.hideOverlay(*_popup);
    refresh();
    if (hadFocus) {
        static_cast<void>(focusWidget(context(), view().screen() != nullptr ? this : nullptr));
    }
}

void MenuButtonImpl::pick(std::size_t index) {
    auto const& level = menuLevel(_entries, _path);
    if (index >= level.size() || !actionable()) {
        return;
    }
    if (!level.at(index).items.empty()) {
        if (level.at(index).enabled) {
            enter(index);
        }
        return;
    }
    auto choice = menuChoice(_entries, _path, index);
    if (!choice) {
        return;
    }
    close();
    auto const handler = _onActivate;
    if (handler) {
        handler(std::move(*choice));
    }
}

void MenuButtonImpl::enter(std::size_t index) {
    _path.push_back(index);
    _popupTop = 0;
    fillPopup(0);
    placePopup();
}

void MenuButtonImpl::leave() {
    if (_path.empty()) {
        return;
    }
    auto const from = _path.back();
    _path.pop_back();
    _popupTop = 0;
    fillPopup(from);
    placePopup();
}

// The highlight starts on @p highlighted, or the first entry after it the cursor can rest on.
void MenuButtonImpl::fillPopup(std::size_t highlighted) {
    auto rows = menuRows(menuLevel(_entries, _path), menuPathEnabled(_entries, _path));
    auto const count = rows.size();
    _popup->setItems(std::move(rows));
    if (count > 0) {
        _popup->setSelectedIndex(std::min(highlighted, count - 1));
        if (!_popup->items().at(_popup->selectedIndex()).enabled) {
            _popup->selectNext();
        }
    }
    refresh();
}

MenuButtonImpl::Placement MenuButtonImpl::placement() const {
    auto const trigger = view().screenBounds();
    auto const viewport = context().screen->viewportArea();
    int const count = static_cast<int>(menuLevel(_entries, _path).size());
    int const below = std::max(0, viewport.y + viewport.height - (trigger.y + 1));
    int const above = std::max(0, trigger.y - viewport.y);
    int const width = popupSize().width;
    Placement place{
        .origin = {.x = std::max(viewport.x, std::min(trigger.x, viewport.x + viewport.width - width)),
                   .y = trigger.y + 1},
        .rows = std::min(count, below)};
    if (below < count && above > below) {
        place.rows = std::min(count, above);
        place.origin.y = trigger.y - place.rows;
    }
    return place;
}

void MenuButtonImpl::placePopup() {
    auto& screen = *context().screen;
    auto const place = placement();
    _popupRows = place.rows;
    if (place.origin != _popupAt || !screen.isOverlayVisible(*_popup)) {
        _popupAt = place.origin;
        screen.showOverlay(*_popup, place.origin);
    }
}

// As wide as the widest entry of any level and the marker before it, or as the trigger.
::core::tui::Size MenuButtonImpl::popupSize() const {
    return {.width = std::max(widestMenuRow(_entries) + 2, view().screenBounds().width), .height = _popupRows};
}

bool MenuButtonImpl::triggerDrawn() const { return view().screen() != nullptr && !view().screenBounds().empty(); }

bool MenuButtonImpl::listUsable() const { return actionable() && triggerDrawn() && _popupRows > 0; }

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

void TabsImpl::setBarShown(bool shown) {
    if (shown != _barShown) {
        _barShown = shown;
        refresh();
    }
}

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
    if (!_barShown) {
        return body;
    }
    return {.width = std::max(bar, body.width), .height = 1 + body.height};
}

std::string TabsImpl::probeText() const { return _selected < _labels.size() ? _labels.at(_selected) : std::string{}; }

// A shown bar keeps its one row, with no blank row under it: a terminal row costs more than the separation earns.
std::vector<::core::tui::Rect> TabsImpl::childAreas(::core::tui::Size size) const {
    int const barRows = _barShown ? 1 : 0;
    return stackAreas(children(),
                      {.x = 0, .y = barRows, .width = size.width, .height = std::max(0, size.height - barRows)},
                      StackSpec{.axis = ui::Axis::Vertical});
}

// The width of the labels from @p first to @p last, both included.
int TabsImpl::labelsWidth(std::size_t first, std::size_t last) const {
    int width = 0;
    for (std::size_t index = first; index <= last && index < _labels.size(); ++index) {
        width += displayWidth(labelText(index));
    }
    return width;
}

// Moves the first drawn label so the highlighted one fits in @p width columns with its markers: `‹` while a label is
// cut before the first, `›` while one is cut after the highlighted.
void TabsImpl::scrollToSelected(int width) {
    if (_labels.empty() || labelsWidth(0, _labels.size() - 1) <= width) {
        _first = 0;
        return;
    }
    std::size_t const target = std::min(_selected, _labels.size() - 1);
    _first = std::min(_first, target);
    int const after = target + 1 < _labels.size() ? 1 : 0;
    while (_first < target && (_first > 0 ? 1 : 0) + labelsWidth(_first, target) + after > width) {
        ++_first;
    }
}

void TabsImpl::paint(::core::tui::Canvas& canvas) {
    auto const& theme = canvas.theme();
    auto const* marked = &theme.textMuted;
    if (actionable()) {
        marked = hasFocus() ? &theme.buttonFocused : &theme.textBold;
    }
    _spans.clear();
    if (_barShown) {
        int const width = canvas.size().width;
        scrollToSelected(width);
        int column = 0;
        if (_first > 0) {
            column += canvas.putString(0, 0, "\u2039", theme.textMuted);
            _spans.push_back(Span{.start = 0, .end = column, .index = 0, .step = -1});
        }
        for (std::size_t index = _first; index < _labels.size(); ++index) {
            std::string const text = labelText(index);
            int const reserve = index + 1 < _labels.size() ? 1 : 0;
            if (column + displayWidth(text) + reserve > width && index != _first) {
                int const marker = std::max(column, width - 1);
                canvas.putString(0, marker, "\u203a", theme.textMuted);
                _spans.push_back(Span{.start = marker, .end = marker + 1, .index = 0, .step = 1});
                break;
            }
            int const start = column;
            column += canvas.putString(0, column, text, index == _selected ? *marked : theme.textMuted);
            _spans.push_back(Span{.start = start, .end = column, .index = index, .step = 0});
        }
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
    if (!_barShown || cell.y != 0 || !focusable()) {
        return;
    }
    // Through what the last paint drew: a marker moves the highlight one tab, as Left and Right do.
    for (Span const& span : _spans) {
        if (cell.x < span.start || cell.x >= span.end) {
            continue;
        }
        if (!focusWidget(context(), this)) {
            return;
        }
        if (span.step < 0 && _selected > 0 && _selected < _labels.size()) {
            select(_selected - 1);
        } else if (span.step > 0 && _selected + 1 < _labels.size()) {
            select(_selected + 1);
        } else if (span.step == 0) {
            select(span.index);
        }
        return;
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

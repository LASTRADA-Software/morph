// SPDX-License-Identifier: Apache-2.0

#include "tui/display_widgets.hpp"

#include <algorithm>
#include <cmath>
#include <core/tui/KeyCode.hpp>
#include <core/tui/Modifier.hpp>
#include <core/tui/Theme.hpp>
#include <morph/ui/theme.hpp>
#include <string>
#include <utility>

namespace morph::tui::detail {

using ::core::tui::EventResult;
using ::core::tui::KeyCode;

namespace {

constexpr std::string_view kDismissal = "[x]";
constexpr int kBarCells = 20;

std::string actionText(std::string_view label) { return "[ " + std::string{label} + " ]"; }

/// A control's style: disabled while out of the user's reach, else by focus.
::core::tui::Style const& controlStyle(::core::tui::Theme const& theme, bool reachable, bool focused) {
    if (!reachable) {
        return theme.buttonDisabled;
    }
    return focused ? theme.buttonFocused : theme.buttonNormal;
}

/// A step's style: the current one accented, else by its state.
::core::tui::Style const& stepStyle(::core::tui::Theme const& theme, ui::StepState state, bool current) {
    if (current) {
        return theme.textAccent;
    }
    if (state == ui::StepState::Failed) {
        return theme.error;
    }
    return state == ui::StepState::Pending ? theme.textMuted : theme.textNormal;
}

bool isPlainKey(::core::tui::KeyEvent const& key, KeyCode code) noexcept {
    return ::core::tui::withoutLockKeys(key.modifiers) == ::core::tui::Modifier::None && key.codepoint == 0 &&
           key.key == code;
}

}  // namespace

::core::tui::Style const& toneStyle(::core::tui::Theme const& theme, ui::Tone tone) {
    if (tone == ui::Tone::Info) {
        return theme.info;
    }
    if (tone == ui::Tone::Ok) {
        return theme.success;
    }
    if (tone == ui::Tone::Warn) {
        return theme.warning;
    }
    if (tone == ui::Tone::Err) {
        return theme.error;
    }
    return theme.textNormal;
}

void BannerImpl::setTone(ui::Tone tone) {
    _tone = tone;
    refresh();
}

void BannerImpl::setText(std::string_view text) {
    _text = std::string{text};
    refresh();
}

void BannerImpl::setActionLabel(std::string_view label) {
    _actionLabel = std::string{label};
    refresh();
}

void BannerImpl::setDismissible(bool dismissible) {
    _dismissible = dismissible;
    refresh();
}

BannerImpl::Controls BannerImpl::controls() const {
    Controls placed;
    int column = displayWidth(_text) + 2;
    if (!_actionLabel.empty()) {
        placed.action = column;
        column += displayWidth(actionText(_actionLabel)) + 1;
    }
    if (_dismissible) {
        placed.dismissal = column;
    }
    return placed;
}

::core::tui::Size BannerImpl::naturalSize() const {
    auto const placed = controls();
    int width = displayWidth(_text);
    if (placed.dismissal) {
        width = *placed.dismissal + static_cast<int>(kDismissal.size());
    } else if (placed.action) {
        width = *placed.action + displayWidth(actionText(_actionLabel));
    }
    return {.width = width, .height = 1};
}

void BannerImpl::paint(::core::tui::Canvas& canvas) {
    auto const& theme = canvas.theme();
    auto const& style = toneStyle(theme, _tone);
    canvas.fill({.x = 0, .y = 0, .width = canvas.width(), .height = 1}, ' ', style);
    canvas.putString(0, 0, _text, style);
    auto const placed = controls();
    auto const& control = controlStyle(theme, actionable(), hasFocus());
    if (placed.action) {
        canvas.putString(0, *placed.action, actionText(_actionLabel), control);
    }
    if (placed.dismissal) {
        canvas.putString(0, *placed.dismissal, kDismissal, control);
    }
}

EventResult BannerImpl::key(::core::tui::KeyEvent const& key) {
    if (isActivation(key) && !_actionLabel.empty()) {
        activate();
        return EventResult::Handled;
    }
    if ((isPlainKey(key, KeyCode::Escape) || isPlainKey(key, KeyCode::Delete)) && _dismissible) {
        dismiss();
        return EventResult::Handled;
    }
    return EventResult::Ignored;
}

void BannerImpl::activate() {
    if (_actionLabel.empty() || !actionable()) {
        return;
    }
    auto const handler = _onAction;
    if (handler) {
        handler();
    }
}

void BannerImpl::dismiss() {
    if (!_dismissible || !actionable()) {
        return;
    }
    auto const handler = _onDismiss;
    if (handler) {
        handler();
    }
}

void BannerImpl::click(::core::tui::Point cell) {
    auto const placed = controls();
    if (placed.action && cell.x >= *placed.action &&
        cell.x < *placed.action + displayWidth(actionText(_actionLabel))) {
        activate();
    } else if (placed.dismissal && cell.x >= *placed.dismissal &&
               cell.x < *placed.dismissal + static_cast<int>(kDismissal.size())) {
        dismiss();
    }
}

void ToastImpl::show(std::string_view text, ui::Tone tone) {
    _text = std::string{text};
    _tone = tone;
    setVisible(true);
    refresh();
}

::core::tui::Size ToastImpl::naturalSize() const { return {.width = displayWidth(_text), .height = 1}; }

void ToastImpl::paint(::core::tui::Canvas& canvas) {
    auto const& style = toneStyle(canvas.theme(), _tone);
    canvas.fill({.x = 0, .y = 0, .width = canvas.width(), .height = 1}, ' ', style);
    canvas.putString(0, 0, _text, style);
}

void BadgeImpl::setTone(ui::Tone tone) {
    _tone = tone;
    refresh();
}

void BadgeImpl::setText(std::string_view text) {
    _text = std::string{text};
    refresh();
}

// A framework icon's glyph before the text; any other icon stays undrawn, as text.
std::string BadgeImpl::caption() const {
    std::string_view const glyph = ui::iconGlyph(_icon);
    return glyph.empty() ? _text : std::string{glyph} + " " + _text;
}

// The remove control's accessible name has no place on a terminal, which names the badge by its text.
void BadgeImpl::setOnRemove(ui::Action onRemove, std::string_view label) {
    static_cast<void>(label);
    _onRemove = std::move(onRemove);
    refresh();
}

// The brackets, and for a removable badge a space and the `×` before the closing one.
::core::tui::Size BadgeImpl::naturalSize() const {
    return {.width = displayWidth(caption()) + (_onRemove ? 4 : 2), .height = 1};
}

void BadgeImpl::paint(::core::tui::Canvas& canvas) {
    ::core::tui::Style style = toneStyle(canvas.theme(), _tone);
    if (_onRemove && hasFocus()) {
        style.inverse = true;
    }
    std::string const glyph{ui::iconGlyph("close")};
    canvas.putString(0, 0, "[" + caption() + (_onRemove ? " " + glyph : std::string{}) + "]", style);
}

EventResult BadgeImpl::key(::core::tui::KeyEvent const& key) {
    if (_onRemove && (isActivation(key) || isPlainKey(key, KeyCode::Delete) || isPlainKey(key, KeyCode::Backspace))) {
        remove();
        return EventResult::Handled;
    }
    return EventResult::Ignored;
}

void BadgeImpl::activate() { remove(); }

// The `×` is the cell before the closing bracket.
void BadgeImpl::click(::core::tui::Point cell) {
    if (_onRemove && cell.y == 0 && cell.x == displayWidth(caption()) + 2) {
        remove();
    }
}

void BadgeImpl::remove() {
    if (!_onRemove || !actionable()) {
        return;
    }
    auto const handler = _onRemove;
    handler();
}

void ProgressImpl::setValue(std::optional<double> value) {
    _value = value;
    refresh();
}

void ProgressImpl::setLabel(std::string_view label) {
    _label = std::string{label};
    refresh();
}

// The bar, the percentage after it (`100%` at the widest), and the label.
::core::tui::Size ProgressImpl::naturalSize() const {
    int const label = _label.empty() ? 0 : displayWidth(_label) + 1;
    return {.width = kBarCells + 2 + 5 + label, .height = 1};
}

// A value outside 0..1 shows at the nearer end, and a NaN at the start.
void ProgressImpl::paint(::core::tui::Canvas& canvas) {
    auto const& theme = canvas.theme();
    std::string bar = "[";
    std::string percent;
    if (_value.has_value()) {
        double const fraction = std::isnan(*_value) ? 0.0 : std::clamp(*_value, 0.0, 1.0);
        auto const filled = static_cast<int>(std::lround(fraction * kBarCells));
        bar.append(static_cast<std::size_t>(filled), '#');
        bar.append(static_cast<std::size_t>(kBarCells - filled), '.');
        percent = std::to_string(std::lround(fraction * 100)) + "%";
    } else {
        bar += "<=>";
        bar.append(static_cast<std::size_t>(kBarCells - 3), '.');
    }
    bar += "]";
    int column = canvas.putString(0, 0, bar, theme.textAccent);
    if (!percent.empty()) {
        column += canvas.putString(0, column, " " + percent, theme.textNormal);
    }
    if (!_label.empty()) {
        canvas.putString(0, column, " " + _label, theme.textNormal);
    }
}

void StepsImpl::setSteps(std::vector<ui::Step> const& steps) {
    _steps = steps;
    refresh();
}

void StepsImpl::setCurrent(std::optional<std::size_t> index) {
    _current = index;
    refresh();
}

::core::tui::Size StepsImpl::naturalSize() const {
    int widest = 0;
    for (auto const& step : _steps) {
        widest = std::max(widest, displayWidth(step.label) + 4);
    }
    return {.width = widest, .height = static_cast<int>(_steps.size())};
}

std::string StepsImpl::probeText() const {
    return _current && *_current < _steps.size() ? _steps.at(*_current).label : std::string{};
}

void StepsImpl::paint(::core::tui::Canvas& canvas) {
    auto const& theme = canvas.theme();
    for (std::size_t index = 0; index < _steps.size() && static_cast<int>(index) < canvas.height(); ++index) {
        auto const& step = _steps.at(index);
        std::string_view mark = "[ ] ";
        if (step.state == ui::StepState::Active) {
            mark = "[>] ";
        } else if (step.state == ui::StepState::Done) {
            mark = "[x] ";
        } else if (step.state == ui::StepState::Failed) {
            mark = "[!] ";
        }
        auto const& style = stepStyle(theme, step.state, _current == index);
        canvas.putString(static_cast<int>(index), 0, std::string{mark} + step.label, style);
    }
}

void KeyValueImpl::setItems(std::vector<ui::KeyValueItem> const& items) {
    _items = items;
    refresh();
}

int KeyValueImpl::captionWidth() const {
    int widest = 0;
    for (auto const& item : _items) {
        widest = std::max(widest, displayWidth(item.label));
    }
    return widest;
}

::core::tui::Size KeyValueImpl::naturalSize() const {
    int widest = 0;
    for (auto const& item : _items) {
        widest = std::max(widest, displayWidth(item.value));
    }
    return {.width = _items.empty() ? 0 : captionWidth() + 2 + widest, .height = static_cast<int>(_items.size())};
}

std::string KeyValueImpl::probeText() const {
    std::string text;
    for (auto const& item : _items) {
        text += (text.empty() ? "" : "\n") + item.label + ": " + item.value;
    }
    return text;
}

void KeyValueImpl::paint(::core::tui::Canvas& canvas) {
    auto const& theme = canvas.theme();
    int const values = captionWidth() + 2;
    for (std::size_t index = 0; index < _items.size() && static_cast<int>(index) < canvas.height(); ++index) {
        auto const row = static_cast<int>(index);
        canvas.putString(row, 0, _items.at(index).label + ":", theme.textMuted);
        canvas.putString(row, values, _items.at(index).value, theme.textNormal);
    }
}

void EmptyStateImpl::setTitle(std::string_view title) {
    _title = std::string{title};
    refresh();
}

void EmptyStateImpl::setText(std::string_view text) {
    _text = std::string{text};
    refresh();
}

void EmptyStateImpl::setActionLabel(std::string_view label) {
    _actionLabel = std::string{label};
    refresh();
}

int EmptyStateImpl::actionRow() const { return (_title.empty() ? 0 : 1) + static_cast<int>(splitLines(_text).size()); }

::core::tui::Size EmptyStateImpl::naturalSize() const {
    int widest = displayWidth(_title);
    for (auto const line : splitLines(_text)) {
        widest = std::max(widest, displayWidth(line));
    }
    if (!_actionLabel.empty()) {
        widest = std::max(widest, displayWidth(actionText(_actionLabel)));
    }
    return {.width = widest, .height = actionRow() + (_actionLabel.empty() ? 0 : 1)};
}

void EmptyStateImpl::paint(::core::tui::Canvas& canvas) {
    auto const& theme = canvas.theme();
    int row = 0;
    if (!_title.empty()) {
        canvas.putString(row++, 0, _title, theme.textBold);
    }
    for (auto const line : splitLines(_text)) {
        canvas.putString(row++, 0, line, theme.textMuted);
    }
    if (!_actionLabel.empty()) {
        auto const& control = controlStyle(theme, actionable(), hasFocus());
        canvas.putString(row, 0, actionText(_actionLabel), control);
    }
}

EventResult EmptyStateImpl::key(::core::tui::KeyEvent const& key) {
    if (!isActivation(key) || _actionLabel.empty()) {
        return EventResult::Ignored;
    }
    activate();
    return EventResult::Handled;
}

void EmptyStateImpl::activate() {
    if (_actionLabel.empty() || !actionable()) {
        return;
    }
    auto const handler = _onAction;
    if (handler) {
        handler();
    }
}

void EmptyStateImpl::click(::core::tui::Point cell) {
    if (cell.y == actionRow() && cell.x < displayWidth(actionText(_actionLabel))) {
        activate();
    }
}

}  // namespace morph::tui::detail

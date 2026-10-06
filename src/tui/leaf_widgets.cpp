// SPDX-License-Identifier: Apache-2.0

#include "tui/leaf_widgets.hpp"

#include <algorithm>
#include <core/tui/Theme.hpp>
#include <memory>
#include <utility>

namespace morph::tui::detail {

namespace {

::core::tui::Style const& roleStyle(::core::tui::Theme const& theme, ui::TextRole role) {
    if (role == ui::TextRole::Muted) {
        return theme.textMuted;
    }
    if (role == ui::TextRole::Heading) {
        return theme.textBold;
    }
    if (role == ui::TextRole::Error) {
        return theme.error;
    }
    if (role == ui::TextRole::Success) {
        return theme.success;
    }
    return theme.textNormal;
}

/// A button's style: disabled while out of the user's reach, else by focus.
::core::tui::Style const& buttonStyle(::core::tui::Theme const& theme, bool reachable, bool focused) {
    if (!reachable) {
        return theme.buttonDisabled;
    }
    return focused ? theme.buttonFocused : theme.buttonNormal;
}

/// A checkbox's style: muted while out of the user's reach, else by focus.
::core::tui::Style const& checkboxStyle(::core::tui::Theme const& theme, bool reachable, bool focused) {
    if (!reachable) {
        return theme.textMuted;
    }
    return focused ? theme.buttonFocused : theme.textNormal;
}

}  // namespace

TextImpl::TextImpl(Context& context) : TuiWidget{context} { adopt(makeView(*this)); }

void TextImpl::setText(std::string_view text) {
    _text = std::string{text};
    refresh();
}

void TextImpl::setRole(ui::TextRole role) {
    _role = role;
    refresh();
}

::core::tui::Size TextImpl::naturalSize() const {
    auto const lines = splitLines(_text);
    int width = 0;
    for (auto const line : lines) {
        width = std::max(width, displayWidth(line));
    }
    return {.width = width, .height = static_cast<int>(lines.size())};
}

void TextImpl::paint(::core::tui::Canvas& canvas) {
    auto const& style = roleStyle(canvas.theme(), _role);
    int row = 0;
    for (auto const line : splitLines(_text)) {
        if (row >= canvas.height()) {
            break;
        }
        canvas.putString(row, 0, line, style);
        ++row;
    }
}

ButtonImpl::ButtonImpl(Context& context) : TuiWidget{context} { adopt(makeView(*this)); }

void ButtonImpl::setLabel(std::string_view label) {
    _label = std::string{label};
    refresh();
}

void ButtonImpl::setOnClick(ui::Action onClick) { _onClick = std::move(onClick); }

::core::tui::Size ButtonImpl::naturalSize() const { return {.width = displayWidth(_label) + 4, .height = 1}; }

void ButtonImpl::paint(::core::tui::Canvas& canvas) {
    canvas.putString(0, 0, "[ " + _label + " ]", buttonStyle(canvas.theme(), actionable(), hasFocus()));
}

::core::tui::EventResult ButtonImpl::key(::core::tui::KeyEvent const& key) {
    if (!isActivation(key)) {
        return ::core::tui::EventResult::Ignored;
    }
    activate();
    return ::core::tui::EventResult::Handled;
}

void ButtonImpl::activate() {
    if (!actionable()) {
        return;
    }
    auto const handler = _onClick;
    if (handler) {
        handler();
    }
}

CheckboxImpl::CheckboxImpl(Context& context) : TuiWidget{context} { adopt(makeView(*this)); }

void CheckboxImpl::setLabel(std::string_view label) {
    _label = std::string{label};
    refresh();
}

void CheckboxImpl::setChecked(bool checked) {
    _checked = checked;
    refresh();
}

void CheckboxImpl::setOnToggle(std::function<void(bool)> onToggle) { _onToggle = std::move(onToggle); }

::core::tui::Size CheckboxImpl::naturalSize() const { return {.width = displayWidth(_label) + 4, .height = 1}; }

void CheckboxImpl::paint(::core::tui::Canvas& canvas) {
    canvas.putString(0, 0, std::string{_checked ? "[x] " : "[ ] "} + _label,
                     checkboxStyle(canvas.theme(), actionable(), hasFocus()));
}

::core::tui::EventResult CheckboxImpl::key(::core::tui::KeyEvent const& key) {
    if (!isActivation(key)) {
        return ::core::tui::EventResult::Ignored;
    }
    activate();
    return ::core::tui::EventResult::Handled;
}

// Optimistic, like a text field: the box shows the new state at once, and a binding that
// disagrees sets it back through setChecked.
void CheckboxImpl::activate() {
    if (!actionable()) {
        return;
    }
    _checked = !_checked;
    refresh();
    auto const handler = _onToggle;
    if (handler) {
        handler(_checked);
    }
}

SpacerImpl::SpacerImpl(Context& context) : TuiWidget{context} { adopt(makeView(*this)); }

}  // namespace morph::tui::detail

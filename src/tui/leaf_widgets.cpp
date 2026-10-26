// SPDX-License-Identifier: Apache-2.0

#include "tui/leaf_widgets.hpp"

#include <algorithm>
#include <array>
#include <core/tui/KeyCode.hpp>
#include <core/tui/Modifier.hpp>
#include <core/tui/Theme.hpp>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <morph/ui/theme.hpp>
#include <string>
#include <string_view>
#include <utility>

#include "tui/drag.hpp"

namespace morph::tui::detail {

namespace {

// A section heading is the muted style, bold: a step below a heading, which is bold body text. `Mono` is body text:
// a terminal draws every cell in one fixed-pitch font already.
::core::tui::Style roleStyle(::core::tui::Theme const& theme, ui::TextRole role) {
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
    if (role == ui::TextRole::Section) {
        ::core::tui::Style section = theme.textMuted;
        section.bold = true;
        return section;
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

/// A list item's style: disabled while out of the user's reach, else focus first, as a list's cursor row is, then
/// selection.
::core::tui::Style const& itemStyle(::core::tui::Theme const& theme, bool reachable, bool focused, bool selected) {
    if (!reachable) {
        return theme.buttonDisabled;
    }
    if (focused) {
        return theme.buttonFocused;
    }
    return selected ? theme.listItemSelected : theme.textNormal;
}

/// A checkbox's or a slider's style: muted while out of the user's reach, else by focus.
::core::tui::Style const& checkboxStyle(::core::tui::Theme const& theme, bool reachable, bool focused) {
    if (!reachable) {
        return theme.textMuted;
    }
    return focused ? theme.buttonFocused : theme.textNormal;
}

}  // namespace

void TextImpl::setText(std::string_view text) {
    _text = std::string{text};
    _runs.clear();
    refresh();
}

void TextImpl::setSpans(std::vector<ui::TextSpan> const& spans) {
    _text = ui::spansText(spans);
    _runs = spans;
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
    // A required text's mark follows its last line, so the cells it takes count, and a row that gives the text its
    // natural width does not draw the mark over its last characters.
    if (required()) {
        width = std::max(width, displayWidth(lines.back()) + displayWidth(kRequiredMark));
    }
    return {.width = width, .height = static_cast<int>(lines.size())};
}

void TextImpl::paint(::core::tui::Canvas& canvas) {
    ::core::tui::Style const style = roleStyle(canvas.theme(), _role);
    if (!_runs.empty()) {
        paintRuns(canvas, _runs, style, true);
    } else {
        int row = 0;
        for (auto const line : splitLines(_text)) {
            if (row >= canvas.height()) {
                break;
            }
            canvas.putString(row, 0, line, style);
            ++row;
        }
    }
    paintMarks(canvas, false);
    // The required mark after the last line's end; given less than that, at the right edge.
    auto const lines = splitLines(_text);
    auto const last = static_cast<int>(lines.size()) - 1;
    if (required() && last < canvas.height()) {
        int const mark = displayWidth(kRequiredMark);
        canvas.putString(last, std::max(0, std::min(displayWidth(lines.back()), canvas.width() - mark)), kRequiredMark,
                         canvas.theme().error);
    }
}

void ButtonImpl::setLabel(std::string_view label) {
    _label = std::string{label};
    refresh();
}

void ButtonImpl::setOnClick(ui::Action onClick) { _onClick = std::move(onClick); }

void ButtonImpl::setEmphasis(ui::ButtonEmphasis emphasis) {
    _emphasis = emphasis;
    refresh();
}

void ButtonImpl::setIcon(std::string_view icon) {
    _icon = std::string{icon};
    refresh();
}

void ButtonImpl::setStyle(ui::ButtonStyle style) {
    _style = style;
    refresh();
}

void ButtonImpl::setSelected(bool selected) {
    _selected = selected;
    refresh();
}

std::string ButtonImpl::caption() const {
    std::string_view const glyph = ui::iconGlyph(_icon);
    return glyph.empty() ? _label : std::string{glyph} + " " + _label;
}

// A push button's brackets and their spaces, or an item's space either side.
::core::tui::Size ButtonImpl::naturalSize() const {
    return {.width = displayWidth(caption()) + (_style == ui::ButtonStyle::Item ? 2 : 4), .height = 1};
}

void ButtonImpl::paint(::core::tui::Canvas& canvas) {
    // Primary is bold, danger bold in the error colour; ghost falls back to Normal's plain style; neither changes a
    // disabled button's look.
    ::core::tui::Style style = buttonStyle(canvas.theme(), actionable(), hasFocus());
    if (actionable() && _emphasis != ui::ButtonEmphasis::Normal && _emphasis != ui::ButtonEmphasis::Ghost) {
        style.bold = true;
        if (_emphasis == ui::ButtonEmphasis::Danger) {
            style.fg = canvas.theme().error.fg;
        }
    }
    if (_style == ui::ButtonStyle::Item) {
        // An item is a line of a list: unbracketed and padded to its slot, so the selected and the focused one read as
        // a highlighted row. Focus wins over selection, as the cursor row of a list does.
        ::core::tui::Theme const& theme = canvas.theme();
        ::core::tui::Style const& line = itemStyle(theme, actionable(), hasFocus(), _selected);
        std::string text = " " + caption();
        text.append(static_cast<std::size_t>(std::max(0, canvas.width() - displayWidth(text))), ' ');
        canvas.putString(0, 0, text, line);
        return;
    }
    // A selected push button draws its caption in the list's selected style and keeps its brackets, so it is as wide
    // as an unselected one and still reads as a button.
    canvas.putString(0, 0, "[ ", style);
    std::string const shown = caption();
    canvas.putString(0, 2, shown, _selected ? canvas.theme().listItemSelected : style);
    canvas.putString(0, 2 + displayWidth(shown), " ]", style);
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
    paintMarks(canvas);
}

// Space only: Enter goes on outwards, so a dialog's default runs from a focused check box as from a field.
::core::tui::EventResult CheckboxImpl::key(::core::tui::KeyEvent const& key) {
    if (::core::tui::withoutLockKeys(key.modifiers) != ::core::tui::Modifier::None || key.codepoint != U' ') {
        return ::core::tui::EventResult::Ignored;
    }
    activate();
    return ::core::tui::EventResult::Handled;
}

// Optimistic, like a text field: the box shows the new state at once, and a binding that
// disagrees sets it back through setChecked.
void CheckboxImpl::activate() {
    if (!actionable() || readOnly()) {
        return;
    }
    _checked = !_checked;
    refresh();
    auto const handler = _onToggle;
    if (handler) {
        handler(_checked);
    }
}

namespace {

// ASCII frames: every terminal draws them, each one cell wide.
constexpr std::array<std::string_view, 4> kSpinner{"|", "/", "-", "\\"};

/// How far @p upper lies above @p lower, which must not exceed it; unsigned, as the two ends of an int64 range may lie
/// further apart than int64 reaches.
std::uint64_t distance(std::int64_t lower, std::int64_t upper) noexcept {
    return static_cast<std::uint64_t>(upper) - static_cast<std::uint64_t>(lower);
}

}  // namespace

BusyImpl::~BusyImpl() {
    std::erase(context().busy, this);
    countSpinners(context());
}

void BusyImpl::setActive(bool active) {
    if (active == _active) {
        return;
    }
    _active = active;
    if (active) {
        context().busy.push_back(this);
    } else {
        std::erase(context().busy, this);
    }
    countSpinners(context());
    refresh();
}

void BusyImpl::setLabel(std::string_view label) {
    _label = std::string{label};
    refresh();
}

::core::tui::Size BusyImpl::naturalSize() const {
    if (!_active) {
        return {.width = 0, .height = 0};
    }
    return {.width = 2 + displayWidth(_label), .height = 1};
}

void BusyImpl::paint(::core::tui::Canvas& canvas) {
    if (!_active) {
        return;
    }
    auto const frame = kSpinner.at(context().animationFrame % kSpinner.size());
    canvas.putString(0, 0, std::string{frame} + " " + _label, canvas.theme().textAccent);
}

void SliderImpl::setRange(std::int64_t minimum, std::int64_t maximum, std::int64_t step) {
    _minimum = minimum;
    _maximum = std::max(minimum, maximum);
    _step = std::max<std::int64_t>(1, step);
    _value = std::clamp(_value, _minimum, _maximum);
    refresh();
}

void SliderImpl::setValue(std::int64_t value) {
    _value = std::clamp(value, _minimum, _maximum);
    refresh();
}

void SliderImpl::setOnChange(std::function<void(std::int64_t)> onChange) { _onChange = std::move(onChange); }

void SliderImpl::setOnCommit(std::function<void(std::int64_t)> onCommit) { _onCommit = std::move(onCommit); }

std::string SliderImpl::probeText() const { return std::to_string(_value); }

std::int64_t SliderImpl::stepAbove() const {
    return distance(_value, _maximum) <= static_cast<std::uint64_t>(_step) ? _maximum : _value + _step;
}

std::int64_t SliderImpl::stepBelow() const {
    return distance(_minimum, _value) <= static_cast<std::uint64_t>(_step) ? _minimum : _value - _step;
}

// Exact while the product fits in 64 bits, which it does for any range a person drags across; past that, close.
int SliderImpl::knobCell(int cells) const {
    auto const span = distance(_minimum, _maximum);
    auto const offset = distance(_minimum, _value);
    auto const scale = static_cast<std::uint64_t>(std::max(0, cells - 1));
    if (span == 0 || scale == 0) {
        return 0;
    }
    std::uint64_t cell = 0;
    if (offset <= std::numeric_limits<std::uint64_t>::max() / scale) {
        cell = offset * scale / span;
    } else {
        cell = static_cast<std::uint64_t>(static_cast<long double>(offset) / static_cast<long double>(span) *
                                          static_cast<long double>(scale));
    }
    return static_cast<int>(std::min(cell, scale));
}

void SliderImpl::paint(::core::tui::Canvas& canvas) {
    auto const label = std::to_string(_value);
    int const track = std::max(3, canvas.width() - static_cast<int>(label.size()) - 3);
    _track = track;
    int const knob = knobCell(track);
    std::string bar = "[";
    bar.append(static_cast<std::size_t>(knob), '=');
    bar += '|';
    bar.append(static_cast<std::size_t>(track - knob - 1), '-');
    bar += "] ";
    bar += label;
    canvas.putString(0, 0, bar, checkboxStyle(canvas.theme(), actionable(), hasFocus()));
    paintMarks(canvas);
}

::core::tui::EventResult SliderImpl::key(::core::tui::KeyEvent const& key) {
    using ::core::tui::KeyCode;
    if (::core::tui::withoutLockKeys(key.modifiers) != ::core::tui::Modifier::None) {
        return ::core::tui::EventResult::Ignored;
    }
    if (key.key == KeyCode::Left) {
        moveTo(stepBelow(), true);
    } else if (key.key == KeyCode::Right) {
        moveTo(stepAbove(), true);
    } else if (key.key == KeyCode::Home) {
        moveTo(_minimum, true);
    } else if (key.key == KeyCode::End) {
        moveTo(_maximum, true);
    } else {
        return ::core::tui::EventResult::Ignored;
    }
    return ::core::tui::EventResult::Handled;
}

// The track starts one cell in, past the "[".
void SliderImpl::pressAt(::core::tui::Point cell) {
    int const position = cell.x - 1;
    _following = position >= 0 && position < _track;
    _moved = false;
    if (_following) {
        moveTo(valueAt(position), false);
    }
}

void SliderImpl::dragTo(::core::tui::Point cell) {
    if (_following) {
        moveTo(valueAt(cell.x - 1), false);
    }
}

// The offset along the range is rounded to the nearest cell's worth and then to the nearest step, unsigned, as the
// range may span all of int64; exact while the product fits in 64 bits. A step past the maximum stops there.
std::int64_t SliderImpl::valueAt(int position) const {
    auto const last = static_cast<std::uint64_t>(std::max(1, _track - 1));
    auto const cell = static_cast<std::uint64_t>(std::clamp(position, 0, std::max(0, _track - 1)));
    auto const span = distance(_minimum, _maximum);
    std::uint64_t offset = 0;
    if (span <= (std::numeric_limits<std::uint64_t>::max() - last) / std::max<std::uint64_t>(cell, 1)) {
        offset = ((span * cell) + (last / 2)) / last;
    } else {
        offset = static_cast<std::uint64_t>(static_cast<long double>(span) * static_cast<long double>(cell) /
                                            static_cast<long double>(last));
    }
    offset = std::min(offset, span);
    auto const step = static_cast<std::uint64_t>(_step);
    auto const below = offset / step * step;
    auto const rest = offset - below;
    auto snapped = below;
    if (rest >= step - rest) {
        snapped = step > span - below ? span : below + step;
    }
    return static_cast<std::int64_t>(static_cast<std::uint64_t>(_minimum) + snapped);
}

// Both handlers may destroy this slider, so they are copied before either runs, and nothing is touched after the
// first call. A pointer move is not finished: it waits for the press to end.
void SliderImpl::moveTo(std::int64_t value, bool finished) {
    auto const next = std::clamp(value, _minimum, _maximum);
    if (next == _value || readOnly()) {
        return;
    }
    _value = next;
    if (!finished) {
        _moved = true;
    }
    refresh();
    auto const onChange = _onChange;
    auto const onCommit = finished ? _onCommit : std::function<void(std::int64_t)>{};
    if (onChange) {
        onChange(next);
    }
    if (onCommit) {
        onCommit(next);
    }
}

bool SliderImpl::pressEnded() {
    bool const moved = _moved;
    _moved = false;
    // A drag key turns the press into a drag-and-drop once the pointer moves, and that gesture's release is the drag
    // controller's: the slider did not finish a move of its own.
    if (!moved || context().drag->dragging()) {
        return false;
    }
    auto const value = _value;
    auto const onCommit = _onCommit;
    if (onCommit) {
        onCommit(value);
    }
    return true;
}

}  // namespace morph::tui::detail

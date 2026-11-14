// SPDX-License-Identifier: Apache-2.0

#include "tui/display_widgets.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <core/tui/KeyCode.hpp>
#include <core/tui/Modifier.hpp>
#include <core/tui/Theme.hpp>
#include <functional>
#include <map>
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
    _runs.clear();
    refresh();
}

void BannerImpl::setSpans(std::vector<ui::TextSpan> const& spans) {
    _text = ui::spansText(spans);
    _runs = spans;
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

void BannerImpl::setActionStyle(ui::BannerActionStyle style, bool stretch) {
    _actionStyle = style;
    _actionStretch = stretch && style != ui::BannerActionStyle::Link;
    refresh();
}

// A link is drawn inside the sentence, at the run marked `action` or else after the text; a button is `[ label ]`
// after the text, or on a row of its own under it when it stretches.
BannerImpl::Controls BannerImpl::controls() const {
    Controls placed;
    int column = displayWidth(_text) + 2;
    if (!_actionLabel.empty()) {
        if (_actionStyle == ui::BannerActionStyle::Link) {
            int start = 0;
            placed.actionWidth = displayWidth(_actionLabel);
            auto const marked = std::ranges::find_if(_runs, [](ui::TextSpan const& run) { return run.action; });
            if (marked != _runs.end()) {
                for (auto run = _runs.begin(); run != marked; ++run) {
                    start += displayWidth(run->text);
                }
                placed.actionWidth = displayWidth(marked->text);
            } else {
                start = displayWidth(_text) + 1;
                column = start + placed.actionWidth + 2;
            }
            placed.action = start;
        } else if (_actionStretch) {
            placed.action = 0;
            placed.actionRow = 1;
            placed.actionWidth = displayWidth(actionText(_actionLabel));
        } else {
            placed.action = column;
            placed.actionWidth = displayWidth(actionText(_actionLabel));
            column += placed.actionWidth + 1;
        }
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
        width = std::max(width, *placed.action + placed.actionWidth);
    }
    return {.width = width, .height = _actionStretch && !_actionLabel.empty() ? 2 : 1};
}

void BannerImpl::paint(::core::tui::Canvas& canvas) {
    auto const& theme = canvas.theme();
    auto const& style = toneStyle(theme, _tone);
    canvas.fill({.x = 0, .y = 0, .width = canvas.width(), .height = canvas.height()}, ' ', style);
    bool const link = _actionStyle == ui::BannerActionStyle::Link && !_actionLabel.empty();
    if (_runs.empty()) {
        canvas.putString(0, 0, _text, style);
    } else {
        paintRuns(canvas, _runs, style, false);
    }
    auto const placed = controls();
    auto const& control = controlStyle(theme, actionable(), hasFocus());
    if (placed.action && link) {
        // The link: underlined in the accent, in the sentence where its run is, else after the text.
        ::core::tui::Style linkStyle = style;
        linkStyle.fg = theme.textAccent.fg;
        linkStyle.underline = true;
        linkStyle.bold = hasFocus();
        bool const inRuns = std::ranges::any_of(_runs, [](ui::TextSpan const& run) { return run.action; });
        if (!inRuns) {
            canvas.putString(0, *placed.action, _actionLabel, linkStyle);
        }
    } else if (placed.action) {
        canvas.putString(placed.actionRow, *placed.action, actionText(_actionLabel), control);
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
    if (placed.action && cell.y == placed.actionRow && cell.x >= *placed.action &&
        cell.x < *placed.action + placed.actionWidth) {
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

std::string BadgeImpl::shownCaption(int width) const {
    if (!_elided) {
        return caption();
    }
    return elidedLine(caption(), width - (_onRemove ? 4 : 2));
}

void BadgeImpl::setElided(bool elided) {
    if (_elided != elided) {
        _elided = elided;
        refresh();
    }
}

void BadgeImpl::paint(::core::tui::Canvas& canvas) {
    ::core::tui::Style style = toneStyle(canvas.theme(), _tone);
    bool const outline = _style != ui::BadgeStyle::Filled;
    if (_onRemove && hasFocus()) {
        style.inverse = true;
    }
    std::string const glyph{ui::iconGlyph("close")};
    std::string const shown = shownCaption(canvas.width());
    _shownWidth = displayWidth(shown);
    canvas.putString(0, 0,
                     (outline ? "(" : "[") + shown + (_onRemove ? " " + glyph : std::string{}) + (outline ? ")" : "]"),
                     style);
}

EventResult BadgeImpl::key(::core::tui::KeyEvent const& key) {
    if (_onRemove && (isActivation(key) || isPlainKey(key, KeyCode::Delete) || isPlainKey(key, KeyCode::Backspace))) {
        remove();
        return EventResult::Handled;
    }
    return EventResult::Ignored;
}

void BadgeImpl::activate() { remove(); }

// The `×` is the cell before the closing bracket, where it was drawn.
void BadgeImpl::click(::core::tui::Point cell) {
    if (_onRemove && cell.y == 0 && cell.x == (_elided ? _shownWidth : displayWidth(caption())) + 2) {
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

void StepsImpl::setLabelled(bool labelled) {
    _labelled = labelled;
    refresh();
}

namespace {

// The cells a step's detail takes after its label: a space and the detail in brackets; none for an empty detail.
int detailWidth(ui::Step const& step) { return step.detail.empty() ? 0 : displayWidth(step.detail) + 3; }

// A step's mark, three cells wide.
std::string_view stepMark(ui::StepState state) {
    if (state == ui::StepState::Active) {
        return "[>]";
    }
    if (state == ui::StepState::Done) {
        return "[x]";
    }
    return state == ui::StepState::Failed ? "[!]" : "[ ]";
}

}  // namespace

::core::tui::Size StepsImpl::naturalSize() const {
    if (!_labelled) {
        // Three cells per mark and at least two of connector between neighbours.
        auto const count = static_cast<int>(_steps.size());
        return {.width = count == 0 ? 0 : (count * 3) + ((count - 1) * 2), .height = count == 0 ? 0 : 1};
    }
    int widest = 0;
    for (auto const& step : _steps) {
        widest = std::max(widest, 4 + displayWidth(step.label) + detailWidth(step));
    }
    return {.width = widest, .height = static_cast<int>(_steps.size())};
}

std::string StepsImpl::probeText() const {
    return _current && *_current < _steps.size() ? _steps.at(*_current).label : std::string{};
}

namespace {

// The marks in one row, the spare width shared among the connectors; a connector after a done step is accented.
void paintMarkRow(::core::tui::Canvas& canvas, std::vector<ui::Step> const& steps,
                  std::optional<std::size_t> current) {
    auto const& theme = canvas.theme();
    auto const count = static_cast<int>(steps.size());
    if (count == 0 || canvas.height() < 1) {
        return;
    }
    int const spare = std::max(canvas.width() - (count * 3), 0);
    int const gaps = std::max(count - 1, 1);
    int column = 0;
    for (int index = 0; index < count; ++index) {
        auto const& step = steps.at(static_cast<std::size_t>(index));
        canvas.putString(0, column, std::string{stepMark(step.state)},
                         stepStyle(theme, step.state, current == static_cast<std::size_t>(index)));
        column += 3;
        if (index + 1 < count) {
            int const length = (spare / gaps) + (index < spare % gaps ? 1 : 0);
            std::string line;
            for (int cell = 0; cell < length; ++cell) {
                line += "─";
            }
            canvas.putString(0, column, line, step.state == ui::StepState::Done ? theme.textAccent : theme.textMuted);
            column += length;
        }
    }
}

// The steps one to a row, each with its mark before the label.
void paintLabelledSteps(::core::tui::Canvas& canvas, std::vector<ui::Step> const& steps,
                        std::optional<std::size_t> current) {
    auto const& theme = canvas.theme();
    for (std::size_t index = 0; index < steps.size() && static_cast<int>(index) < canvas.height(); ++index) {
        auto const& step = steps.at(index);
        std::string_view mark = "[ ] ";
        if (step.state == ui::StepState::Active) {
            mark = "[>] ";
        } else if (step.state == ui::StepState::Done) {
            mark = "[x] ";
        } else if (step.state == ui::StepState::Failed) {
            mark = "[!] ";
        }
        auto const& style = stepStyle(theme, step.state, current == index);
        auto const row = static_cast<int>(index);
        canvas.putString(row, 0, std::string{mark} + step.label, style);
        // The detail is dropped, whole, from a row too narrow for it; the canvas cuts the label.
        int const labelEnd = 4 + displayWidth(step.label);
        if (detailWidth(step) > 0 && labelEnd + detailWidth(step) <= canvas.width()) {
            canvas.putString(row, labelEnd, " [" + step.detail + "]", theme.textMuted);
        }
    }
}

}  // namespace

void StepsImpl::paint(::core::tui::Canvas& canvas) {
    if (_labelled) {
        paintLabelledSteps(canvas, _steps, _current);
    } else {
        paintMarkRow(canvas, _steps, _current);
    }
}

namespace {

// The characters an item's value shows: its runs joined, or its text.
std::string shownValue(ui::KeyValueItem const& item) {
    return item.spans.empty() ? item.value : ui::spansText(item.spans);
}

}  // namespace

void KeyValueImpl::setItems(std::vector<ui::KeyValueItem> const& items) {
    _items = items;
    refresh();
}

void KeyValueImpl::setCaptionPlacement(ui::CaptionPlacement placement) {
    _captions = placement;
    refresh();
}

void KeyValueImpl::setCaptionWidth(int units) {
    _captionWidth = units;
    refresh();
}

// The caption column's width: the one the node sets, else the widest caption.
int KeyValueImpl::captionWidth() const {
    if (_captionWidth) {
        return *_captionWidth;
    }
    int widest = 0;
    for (auto const& item : _items) {
        widest = std::max(widest, displayWidth(item.label));
    }
    return widest;
}

// Stacked, each pair is two rows and no rule: a rule would cost a row per pair on a 24-row screen, and the muted
// caption already separates the pairs.
::core::tui::Size KeyValueImpl::naturalSize() const {
    int widest = 0;
    for (auto const& item : _items) {
        widest = std::max(widest, displayWidth(shownValue(item)));
    }
    auto const pairs = static_cast<int>(_items.size());
    if (_captions == ui::CaptionPlacement::Above) {
        return {.width = std::max(widest, captionWidth()), .height = 2 * pairs};
    }
    return {.width = _items.empty() ? 0 : captionWidth() + 2 + widest, .height = pairs};
}

std::string KeyValueImpl::probeText() const {
    std::string text;
    for (auto const& item : _items) {
        text += (text.empty() ? "" : "\n") + item.label + ": " + shownValue(item);
    }
    return text;
}

// A toned value draws in its tone's style, bold.
void KeyValueImpl::paint(::core::tui::Canvas& canvas) {
    auto const& theme = canvas.theme();
    bool const above = _captions == ui::CaptionPlacement::Above;
    int const values = above ? 0 : captionWidth() + 2;
    for (std::size_t index = 0; index < _items.size(); ++index) {
        ui::KeyValueItem const& item = _items.at(index);
        int const row = above ? 2 * static_cast<int>(index) : static_cast<int>(index);
        int const valueRow = above ? row + 1 : row;
        if (valueRow >= canvas.height()) {
            break;
        }
        // Beside, the caption and its colon take the column and one cell: a longer one is cut with an ellipsis, since
        // a cell row cannot wrap.
        std::string const caption = above ? item.label : elidedLine(item.label + ":", captionWidth() + 1);
        canvas.putString(row, 0, caption, theme.textMuted);
        ::core::tui::Style style = item.tone == ui::Tone::Neutral ? theme.textNormal : toneStyle(theme, item.tone);
        style.bold = style.bold || item.tone != ui::Tone::Neutral;
        if (item.spans.empty()) {
            canvas.putString(valueRow, values, item.value, style);
        } else {
            paintRuns(canvas, item.spans, style, false, valueRow, values);
        }
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

namespace morph::tui::detail {

namespace {

// Smaller than this, a plot says too little; the summary says it instead.
constexpr int kChartMinColumns = 20;
constexpr int kChartMinRows = 5;
constexpr int kChartGutterLimit = 8;
constexpr std::array<std::string_view, 4> kSeriesGlyphs = {"●", "■", "▲", "◆"};
// A bar's end cell: the lower 1 to 7 eighths of the cell.
constexpr std::array<std::string_view, 8> kLowerBlocks = {"", "▁", "▂", "▃", "▄", "▅", "▆", "▇"};
constexpr double kBarShare = 0.8;

/// A series' style: normal text in `color.chart.<n>`'s default, in token order.
::core::tui::Style seriesStyle(::core::tui::Theme const& theme, std::size_t index) {
    static constexpr std::array<std::string_view, 4> kTokens = {"color.chart.1", "color.chart.2", "color.chart.3",
                                                                "color.chart.4"};
    ::core::tui::Style style = theme.textNormal;
    std::string_view const token = kTokens.at(index % kTokens.size());
    for (ui::ThemeTokenSpec const& spec : ui::kThemeTokens) {
        if (spec.path == token) {
            if (std::optional<ui::Rgba> const colour = ui::detail::hexColor(spec.color)) {
                style.fg = ::core::tui::RgbColor{.r = colour->r, .g = colour->g, .b = colour->b};
            }
        }
    }
    return style;
}

/// A line's tone style: a neutral line is muted, as on Qt Quick.
::core::tui::Style const& lineStyle(::core::tui::Theme const& theme, ui::Tone tone) {
    return tone == ui::Tone::Neutral ? theme.textMuted : toneStyle(theme, tone);
}

/// How much a line's tone says: an error more than a warning, and any tone more than none.
int severity(ui::Tone tone) noexcept {
    switch (tone) {
        case ui::Tone::Err:
            return 4;
        case ui::Tone::Warn:
            return 3;
        case ui::Tone::Ok:
            return 2;
        case ui::Tone::Info:
            return 1;
        case ui::Tone::Neutral:
        default:
            return 0;
    }
}

/// The longest start of @p text at most @p width cells wide, cut between characters.
std::string fitted(std::string_view text, int width) {
    std::size_t end = 0;
    while (end < text.size()) {
        std::size_t next = end + 1;
        while (next < text.size() && (static_cast<unsigned char>(text.at(next)) & 0xC0U) == 0x80U) {
            ++next;
        }
        if (displayWidth(text.substr(0, next)) > width) {
            break;
        }
        end = next;
    }
    return std::string{text.substr(0, end)};
}

/// @p text broken into lines of at most @p width cells at spaces; a word wider than a line is cut.
std::vector<std::string> wrapped(std::string_view text, int width) {
    std::vector<std::string> lines;
    std::string line;
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t const end = std::min(text.find(' ', start), text.size());
        std::string const word{text.substr(start, end - start)};
        std::string joined = line;
        if (!joined.empty()) {
            joined += ' ';
        }
        joined += word;
        if (!line.empty() && displayWidth(joined) > width) {
            lines.push_back(line);
            line = word;
        } else {
            line = joined;
        }
        start = end + 1;
    }
    if (!line.empty()) {
        lines.push_back(line);
    }
    return lines;
}

}  // namespace

void ChartImpl::setData(ui::ChartData const& data) {
    _data = data;
    refresh();
}

::core::tui::Size ChartImpl::naturalSize() const { return {.width = 40, .height = 8}; }

int ChartImpl::Plot::rowOf(ui::ChartData const& data, double value) const {
    double const span = data.yMax - data.yMin;
    double const share = span > 0 ? (data.yMax - value) / span : 0.5;
    return top + static_cast<int>(std::lround(std::clamp(share, 0.0, 1.0) * (rows - 1)));
}

int ChartImpl::Plot::columnOf(ui::ChartData const& data, double position) const {
    double const span = data.xMax - data.xMin;
    double const share = span > 0 ? (position - data.xMin) / span : 0.5;
    return left + static_cast<int>(std::lround(std::clamp(share, 0.0, 1.0) * (columns - 1)));
}

void ChartImpl::paintSummary(::core::tui::Canvas& canvas) const {
    std::vector<std::string> const lines = wrapped(_data.summary, std::max(1, canvas.width()));
    for (std::size_t index = 0; index < lines.size() && static_cast<int>(index) < canvas.height(); ++index) {
        canvas.putString(static_cast<int>(index), 0, lines.at(index), canvas.theme().textMuted);
    }
}

struct ChartImpl::BarLayout {
    struct Cell {
        std::string_view glyph;
        ::core::tui::Style style;
    };
    /// A cell a bar marks beyond its tip, and the note beside it.
    struct Mark {
        int row = 0;
        int column = 0;
        std::string_view glyph;
        ::core::tui::Style style;
        int noteRow = 0;
        int noteColumn = 0;
        std::string note;
    };
    std::map<std::pair<int, int>, Cell> cells;
    std::vector<Mark> marks;
};

// A bar's cells are laid out in eighths of a row from a baseline on a row boundary. The tip's height uses the same
// linear map as `rowOf`, so a bar at a tick's value ends in the tick's row. Where bars share a cell the one whose
// tip is nearer the baseline is drawn last, so it is in front.
ChartImpl::BarLayout ChartImpl::barLayout(::core::tui::Theme const& theme, Plot const& plot) const {
    BarLayout layout;
    std::vector<std::size_t> barSeries;
    for (std::size_t index = 0; index < _data.series.size(); ++index) {
        if (_data.series.at(index).style == ui::ChartStyle::Bars) {
            barSeries.push_back(index);
        }
    }
    double const span = _data.yMax - _data.yMin;
    if (barSeries.empty() || span <= 0 || _data.xMax <= _data.xMin || plot.columns < 1) {
        return layout;
    }
    int const rows = plot.rows;
    auto const heightOf = [&](double value) { return ((rows - 1) * (value - _data.yMin) / span) + 0.5; };
    double const baseline = std::clamp(0.0, _data.yMin, _data.yMax);
    int boundary = 0;
    if (baseline >= _data.yMax) {
        boundary = rows;
    } else if (baseline > _data.yMin) {
        boundary = static_cast<int>(std::floor(heightOf(baseline) + 0.5));
    }
    auto const count = static_cast<int>(barSeries.size());
    double const slotColumns = _data.barSlot / (_data.xMax - _data.xMin) * (plot.columns - 1);
    int const width = std::max(1, static_cast<int>(std::floor(kBarShare * slotColumns / count)));
    struct Bar {
        std::size_t series;
        ui::ChartPoint const* point;
        int firstColumn;
        int eighths;  // signed: up from the baseline when positive
    };
    std::vector<Bar> bars;
    for (int place = 0; place < count; ++place) {
        std::size_t const series = barSeries.at(static_cast<std::size_t>(place));
        for (ui::ChartPoint const& point : _data.series.at(series).points) {
            if (!point.y.has_value()) {
                continue;
            }
            int eighths = 0;
            if (*point.y > baseline) {
                eighths = std::max(1, static_cast<int>(std::lround(8 * (heightOf(*point.y) - boundary))));
                eighths = std::min(eighths, (rows - boundary) * 8);
            } else if (*point.y < baseline) {
                eighths = -std::max(1, static_cast<int>(std::lround(8 * (boundary - heightOf(*point.y)))));
                eighths = std::max(eighths, -boundary * 8);
            }
            bars.push_back(Bar{.series = series,
                               .point = &point,
                               .firstColumn = plot.columnOf(_data, point.x) - ((count * width) / 2) + (place * width),
                               .eighths = eighths});
        }
    }
    std::ranges::stable_sort(bars,
                             [](Bar const& a, Bar const& b) { return std::abs(a.eighths) > std::abs(b.eighths); });
    auto const inPlotColumns = [&plot](int column) {
        return column >= plot.left && column < plot.left + plot.columns;
    };
    for (Bar const& bar : bars) {
        ui::ChartPoint const& point = *bar.point;
        ::core::tui::Style const style =
            point.tone.has_value() ? toneStyle(theme, *point.tone) : seriesStyle(theme, bar.series);
        // A bar of the baseline's own value has no extent; it counts as upward, as on Qt Quick, unless it is clipped
        // below.
        bool const up = bar.eighths > 0 || (bar.eighths == 0 && point.clip != ui::ChartClip::Below);
        int const extent = std::abs(bar.eighths);
        // Cell k counts from the plot's bottom edge.
        auto const rowOfCell = [&](int cell) { return plot.top + rows - 1 - cell; };
        int const full = extent / 8;
        int const part = extent % 8;
        for (int column = bar.firstColumn; column < bar.firstColumn + width; ++column) {
            if (!inPlotColumns(column)) {
                continue;
            }
            for (int step = 0; step < full; ++step) {
                layout.cells[{rowOfCell(up ? boundary + step : boundary - 1 - step), column}] = {"█", style};
            }
            if (part > 0) {
                ::core::tui::Style end = style;
                // A bar growing down covers the top of its end cell: the lower block of what it leaves uncovered,
                // reversed, so the terminal's own colours fill that part.
                end.inverse = !up;
                layout.cells[{rowOfCell(up ? boundary + full : boundary - 1 - full), column}] = {
                    kLowerBlocks.at(static_cast<std::size_t>(up ? part : 8 - part)), end};
            }
        }
        int tip = up ? boundary + ((extent - 1) / 8) : boundary - 1 - ((extent - 1) / 8);
        if (extent == 0) {
            // The cell beside the baseline, which is where the cross goes; a clipped one is at the axis end.
            tip = std::clamp(up ? boundary : boundary - 1, 0, rows - 1);
            if (point.clip != ui::ChartClip::None) {
                tip = point.clip == ui::ChartClip::Above ? rows - 1 : 0;
            }
        }
        int const beyond = up ? tip + 1 : tip - 1;
        bool const marked = point.tone.has_value() && point.clip == ui::ChartClip::None;
        int markCell = tip;
        if (marked && extent > 0 && beyond >= 0 && beyond < rows) {
            markCell = beyond;
        }
        int const lastColumn = bar.firstColumn + width - 1;
        if (point.clip != ui::ChartClip::None || marked) {
            layout.marks.push_back(BarLayout::Mark{
                .row = rowOfCell(markCell),
                .column = std::clamp(bar.firstColumn + ((width - 1) / 2), plot.left, plot.left + plot.columns - 1),
                .glyph = point.clip == ui::ChartClip::Above ? "△" : (point.clip == ui::ChartClip::Below ? "▽" : "✕"),
                .style = style,
                .noteRow = rowOfCell(markCell),
                .noteColumn = lastColumn + 2,
                .note = point.note});
        } else if (!point.note.empty()) {
            layout.marks.push_back(BarLayout::Mark{.row = -1,
                                                   .column = 0,
                                                   .glyph = {},
                                                   .style = style,
                                                   .noteRow = rowOfCell(tip),
                                                   .noteColumn = lastColumn + 2,
                                                   .note = point.note});
        }
    }
    return layout;
}

// A line's row is drawn once, by the most severe line on it; its label is that line's. A cell a bar covers keeps the
// bar, a terminal cell holding one glyph; the label is written over it.
void ChartImpl::paintLines(::core::tui::Canvas& canvas, Plot const& plot, bool byLines, BarLayout const& bars) const {
    auto const& theme = canvas.theme();
    std::vector<ui::ChartLine const*> rows(static_cast<std::size_t>(plot.top + plot.rows), nullptr);
    for (ui::ChartLine const& line : _data.lines) {
        ui::ChartLine const*& slot = rows.at(static_cast<std::size_t>(plot.rowOf(_data, line.value)));
        if (slot == nullptr || severity(line.tone) > severity(slot->tone)) {
            slot = &line;
        }
    }
    if (!byLines) {
        for (ui::ChartTick const& tick : _data.yTicks) {
            std::string const text = fitted(tick.text, plot.left - 1);
            canvas.putString(plot.rowOf(_data, tick.value), plot.left - 1 - displayWidth(text), text, theme.textMuted);
        }
    }
    for (std::size_t row = 0; row < rows.size(); ++row) {
        ui::ChartLine const* const line = rows.at(row);
        if (line == nullptr) {
            continue;
        }
        std::string_view glyph = "─";
        if (line->style == ui::ChartLineStyle::Dashed) {
            glyph = "╌";
        } else if (line->style == ui::ChartLineStyle::Dotted) {
            glyph = "┄";
        }
        auto const& style = lineStyle(theme, line->tone);
        for (int column = plot.left; column < plot.left + plot.columns; ++column) {
            if (!bars.cells.contains({static_cast<int>(row), column})) {
                canvas.put(static_cast<int>(row), column, glyph, style);
            }
        }
        if (byLines) {
            std::string const text = fitted(line->label, plot.left - 1);
            canvas.putString(static_cast<int>(row), plot.left - 1 - displayWidth(text), text, style);
        } else {
            int const width = std::min(displayWidth(line->label), plot.columns);
            canvas.putString(static_cast<int>(row), plot.left + plot.columns - width, line->label, style);
        }
    }
}

// The marks beyond the bars' tips, and the bars' notes, when they fit.
void ChartImpl::paintBarMarks(::core::tui::Canvas& canvas, Plot const& plot, BarLayout const& bars) const {
    for (BarLayout::Mark const& mark : bars.marks) {
        if (mark.row >= 0) {
            canvas.put(mark.row, mark.column, mark.glyph, mark.style);
        }
        int const room = plot.left + plot.columns - mark.noteColumn;
        if (!mark.note.empty() && displayWidth(mark.note) <= room) {
            canvas.putString(mark.noteRow, mark.noteColumn, mark.note, mark.style);
        }
    }
}

// A cell holding two points shows the marked one, else the later.
void ChartImpl::paintPoints(::core::tui::Canvas& canvas, Plot const& plot) const {
    auto const& theme = canvas.theme();
    struct Cell {
        int row;
        int column;
        std::size_t series;
        ui::ChartPoint const* point;
    };
    std::vector<Cell> cells;
    for (std::size_t series = 0; series < _data.series.size(); ++series) {
        if (_data.series.at(series).style == ui::ChartStyle::Bars) {
            continue;
        }
        for (ui::ChartPoint const& point : _data.series.at(series).points) {
            if (!point.y.has_value()) {
                continue;
            }
            Cell const cell{.row = plot.rowOf(_data, *point.y),
                            .column = plot.columnOf(_data, point.x),
                            .series = series,
                            .point = &point};
            auto const taken = std::ranges::find_if(
                cells, [&cell](Cell const& other) { return other.row == cell.row && other.column == cell.column; });
            if (taken == cells.end()) {
                cells.push_back(cell);
            } else if (point.tone.has_value() || !taken->point->tone.has_value()) {
                *taken = cell;
            }
        }
    }
    for (Cell const& cell : cells) {
        ui::ChartPoint const& point = *cell.point;
        ::core::tui::Style const style =
            point.tone.has_value() ? toneStyle(theme, *point.tone) : seriesStyle(theme, cell.series);
        std::string_view glyph = kSeriesGlyphs.at(cell.series % kSeriesGlyphs.size());
        if (point.clip == ui::ChartClip::Above) {
            glyph = "△";
        } else if (point.clip == ui::ChartClip::Below) {
            glyph = "▽";
        } else if (point.tone.has_value()) {
            glyph = "✕";
        }
        canvas.put(cell.row, cell.column, glyph, style);
        int const room = plot.left + plot.columns - (cell.column + 2);
        if (!point.note.empty() && displayWidth(point.note) <= room) {
            canvas.putString(cell.row, cell.column + 2, point.note, style);
        }
    }
}

// The points' labels, each skipped when it would overlap the last one drawn and the newest always drawn, then the
// legend for two or more series.
void ChartImpl::paintBelow(::core::tui::Canvas& canvas, Plot const& plot) const {
    auto const& theme = canvas.theme();
    int row = plot.top + plot.rows;
    std::vector<std::pair<int, std::string const*>> labels;
    for (ui::ChartSeries const& series : _data.series) {
        for (ui::ChartPoint const& point : series.points) {
            if (!point.xLabel.empty()) {
                labels.emplace_back(plot.columnOf(_data, point.x), &point.xLabel);
            }
        }
    }
    if (!labels.empty()) {
        std::ranges::stable_sort(labels, {}, &std::pair<int, std::string const*>::first);
        auto const [lastColumn, lastText] = labels.back();
        int const newestStart = std::max(plot.left, lastColumn - (displayWidth(*lastText) / 2));
        int reach = 0;
        for (std::size_t index = 0; index + 1 < labels.size(); ++index) {
            auto const [column, text] = labels.at(index);
            int const start = std::max(plot.left, column - (displayWidth(*text) / 2));
            int const end = start + displayWidth(*text);
            if (start < reach || end >= newestStart) {
                continue;
            }
            canvas.putString(row, start, *text, theme.textMuted);
            reach = end + 1;
        }
        canvas.putString(row, newestStart, *lastText, theme.textMuted);
        ++row;
    }
    if (_data.series.size() >= 2) {
        int column = plot.left;
        for (std::size_t index = 0; index < _data.series.size(); ++index) {
            canvas.put(row, column,
                       _data.series.at(index).style == ui::ChartStyle::Bars
                           ? "█"
                           : kSeriesGlyphs.at(index % kSeriesGlyphs.size()),
                       seriesStyle(theme, index));
            column += 2;
            column += canvas.putString(row, column, _data.series.at(index).label, theme.textNormal) + 2;
        }
    }
}

void ChartImpl::paint(::core::tui::Canvas& canvas) {
    if (canvas.width() < kChartMinColumns || canvas.height() < kChartMinRows) {
        paintSummary(canvas);
        return;
    }
    bool const byLines = _data.lines.size() >= 3;
    int gutter = 0;
    if (byLines) {
        for (ui::ChartLine const& line : _data.lines) {
            gutter = std::max(gutter, displayWidth(line.label));
        }
    } else {
        for (ui::ChartTick const& tick : _data.yTicks) {
            gutter = std::max(gutter, displayWidth(tick.text));
        }
    }
    bool const hasLabels = std::ranges::any_of(_data.series, [](ui::ChartSeries const& series) {
        return std::ranges::any_of(series.points, [](ui::ChartPoint const& point) { return !point.xLabel.empty(); });
    });
    Plot plot;
    plot.top = _data.yLabel.empty() ? 0 : 1;
    plot.left = std::min(gutter, kChartGutterLimit) + 1;
    plot.columns = canvas.width() - plot.left;
    plot.rows = canvas.height() - plot.top - (hasLabels ? 1 : 0) - (_data.series.size() >= 2 ? 1 : 0);
    if (plot.rows < 2) {
        paintSummary(canvas);
        return;
    }
    if (!_data.yLabel.empty()) {
        canvas.putString(0, 0, _data.yLabel, canvas.theme().textMuted);
    }
    BarLayout const bars = barLayout(canvas.theme(), plot);
    for (auto const& [position, cell] : bars.cells) {
        canvas.put(position.first, position.second, cell.glyph, cell.style);
    }
    paintLines(canvas, plot, byLines, bars);
    paintBarMarks(canvas, plot, bars);
    paintPoints(canvas, plot);
    paintBelow(canvas, plot);
}

}  // namespace morph::tui::detail

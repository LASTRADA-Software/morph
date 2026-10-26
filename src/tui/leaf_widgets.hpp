// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <cstdint>
#include <functional>
#include <morph/ui/backend.hpp>
#include <string>
#include <string_view>

#include "tui/widget.hpp"

namespace morph::tui::detail {

/// Text: one row per line, styled by role.
class TextImpl final : public TuiField<ui::TextWidget> {
public:
    explicit TextImpl(Context& context) : TuiField{context} { adopt(makeView(*this)); }
    void setText(std::string_view text) override;
    void setRole(ui::TextRole role) override;
    void setSpans(std::vector<ui::TextSpan> const& spans) override;
    /// A text is not described; the mount never asks.
    void setDescription(std::string_view description) override;
    /// A text is not toned; the mount never asks.
    void setTone(ui::Tone tone) override;
    [[nodiscard]] ::core::tui::Size controlSize() const override;
    [[nodiscard]] std::string probeText() const override { return _text; }
    [[nodiscard]] bool placedAcrossColumn() const override { return true; }
    void paintControl(::core::tui::Canvas& canvas) override;

private:
    // The characters shown: the text, or the runs joined.
    std::string _text;
    // The runs, while the text has spans; empty for a plain text.
    std::vector<ui::TextSpan> _runs;
    ui::TextRole _role = ui::TextRole::Normal;
};

/// Button: `[ label ]`, activated by Enter, Space or a click; a list item draws ` label` across its whole slot.
class ButtonImpl final : public TuiWidget<ui::ButtonWidget> {
public:
    explicit ButtonImpl(Context& context) : TuiWidget{context} { adopt(makeView(*this)); }
    void setLabel(std::string_view label) override;
    void setOnClick(ui::Action onClick) override;
    void setEmphasis(ui::ButtonEmphasis emphasis) override;
    void setIcon(std::string_view icon) override;
    void setStyle(ui::ButtonStyle style) override;
    void setSelected(bool selected) override;
    [[nodiscard]] ::core::tui::Size naturalSize() const override;
    [[nodiscard]] bool wantsFocus() const override { return true; }
    [[nodiscard]] std::string probeText() const override { return _label; }
    /// A push button keeps its label's width; a list item fills its slot.
    [[nodiscard]] bool placedAcrossColumn() const override { return _style == ui::ButtonStyle::Push; }
    void paint(::core::tui::Canvas& canvas) override;
    [[nodiscard]] ::core::tui::EventResult key(::core::tui::KeyEvent const& key) override;
    void activate() override;

private:
    // The caption drawn: a framework icon's glyph before the label; any other icon stays undrawn, as text.
    [[nodiscard]] std::string caption() const;
    std::string _label;
    std::string _icon;
    ui::Action _onClick;
    ui::ButtonEmphasis _emphasis = ui::ButtonEmphasis::Normal;
    ui::ButtonStyle _style = ui::ButtonStyle::Push;
    bool _selected = false;
};

/// Checkbox: `[x] label`; Enter, Space or a click flips it and reports the new state.
class CheckboxImpl final : public TuiField<ui::CheckboxWidget> {
public:
    explicit CheckboxImpl(Context& context) : TuiField{context} { adopt(makeView(*this)); }
    void setLabel(std::string_view label) override;
    void setChecked(bool checked) override;
    void setOnToggle(std::function<void(bool)> onToggle) override;
    [[nodiscard]] ::core::tui::Size controlSize() const override;
    [[nodiscard]] bool wantsFocus() const override { return true; }
    [[nodiscard]] std::string probeText() const override { return _label; }
    [[nodiscard]] bool placedAcrossColumn() const override { return true; }
    void paintControl(::core::tui::Canvas& canvas) override;
    [[nodiscard]] ::core::tui::EventResult key(::core::tui::KeyEvent const& key) override;
    void activate() override;

private:
    std::string _label;
    bool _checked = false;
    std::function<void(bool)> _onToggle;
};

/// Spacer: empty; in a stack a Content-sized spacer stretches.
class SpacerImpl final : public TuiWidget<ui::SpacerWidget> {
public:
    explicit SpacerImpl(Context& context) : TuiWidget{context} { adopt(makeView(*this)); }
    [[nodiscard]] ::core::tui::Size naturalSize() const override { return {.width = 0, .height = 0}; }
    [[nodiscard]] bool expandsByDefault() const override { return true; }
};

/// Busy: a spinner and its label while active, nothing at all otherwise. Each `Context::animationFrame` draws the
/// next spinner frame; the frontend advances it on a timer while `Context::activeBusy` counts an active Busy the user
/// could see.
class BusyImpl final : public TuiWidget<ui::BusyWidget> {
public:
    explicit BusyImpl(Context& context) : TuiWidget{context} { adopt(makeView(*this)); }
    /// @brief Leaves the active Busy widgets.
    ~BusyImpl() override;
    BusyImpl(BusyImpl const&) = delete;
    BusyImpl& operator=(BusyImpl const&) = delete;
    BusyImpl(BusyImpl&&) = delete;
    BusyImpl& operator=(BusyImpl&&) = delete;

    void setActive(bool active) override;
    void setLabel(std::string_view label) override;
    [[nodiscard]] ::core::tui::Size naturalSize() const override;
    [[nodiscard]] std::string probeText() const override { return _label; }
    void paint(::core::tui::Canvas& canvas) override;

private:
    bool _active = false;
    std::string _label;
};

/// Slider: `[====|----] value`. Left and Right move by one step, Home and End to the ends. A press on the track
/// moves to the value at that cell, to the nearest step, and while the press lasts the value follows the pointer
/// along the track, held at either end past it; a press elsewhere moves nothing. Each move reports the new value once
/// it changed; a key step is a finished move and reports the commit too, and a drag reports the commit when the press
/// ends. No setter reports.
class SliderImpl final : public TuiField<ui::SliderWidget> {
public:
    explicit SliderImpl(Context& context) : TuiField{context} { adopt(makeView(*this)); }
    void setRange(std::int64_t minimum, std::int64_t maximum, std::int64_t step) override;
    void setValue(std::int64_t value) override;
    void setOnChange(std::function<void(std::int64_t)> onChange) override;
    void setOnCommit(std::function<void(std::int64_t)> onCommit) override;
    [[nodiscard]] ::core::tui::Size controlSize() const override { return {.width = 24, .height = 1}; }
    [[nodiscard]] bool wantsFocus() const override { return true; }
    [[nodiscard]] std::string probeText() const override;
    void paintControl(::core::tui::Canvas& canvas) override;
    [[nodiscard]] ::core::tui::EventResult key(::core::tui::KeyEvent const& key) override;
    void pressAt(::core::tui::Point cell) override;
    void dragTo(::core::tui::Point cell) override;
    [[nodiscard]] bool pressEnded() override;

private:
    /// The value at track cell @p position (0-based, held to the track), to the nearest step from the minimum.
    [[nodiscard]] std::int64_t valueAt(int position) const;
    /// The value one step above the current one, or the maximum when that is closer.
    [[nodiscard]] std::int64_t stepAbove() const;
    /// The value one step below the current one, or the minimum when that is closer.
    [[nodiscard]] std::int64_t stepBelow() const;
    /// The track cell of the current value, among @p cells.
    [[nodiscard]] int knobCell(int cells) const;
    /// Moves to @p value and reports the change; a @p finished move reports the commit too.
    void moveTo(std::int64_t value, bool finished);

    std::int64_t _minimum = 0;
    std::int64_t _maximum = 100;
    std::int64_t _step = 1;
    std::int64_t _value = 0;
    int _track = 0;           ///< How many cells the track had in the last frame.
    bool _following = false;  ///< Whether the press that lasts began on the track.
    bool _moved = false;      ///< Whether the press that lasts moved the value.
    std::function<void(std::int64_t)> _onChange;
    std::function<void(std::int64_t)> _onCommit;
};

}  // namespace morph::tui::detail

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
class TextImpl final : public TuiWidget<ui::TextWidget> {
public:
    explicit TextImpl(Context& context) : TuiWidget{context} { adopt(makeView(*this)); }
    void setText(std::string_view text) override;
    void setRole(ui::TextRole role) override;
    [[nodiscard]] ::core::tui::Size naturalSize() const override;
    [[nodiscard]] std::string probeText() const override { return _text; }
    void paint(::core::tui::Canvas& canvas) override;

private:
    std::string _text;
    ui::TextRole _role = ui::TextRole::Normal;
};

/// Button: `[ label ]`, activated by Enter, Space or a click.
class ButtonImpl final : public TuiWidget<ui::ButtonWidget> {
public:
    explicit ButtonImpl(Context& context) : TuiWidget{context} { adopt(makeView(*this)); }
    void setLabel(std::string_view label) override;
    void setOnClick(ui::Action onClick) override;
    [[nodiscard]] ::core::tui::Size naturalSize() const override;
    [[nodiscard]] bool wantsFocus() const override { return true; }
    [[nodiscard]] std::string probeText() const override { return _label; }
    void paint(::core::tui::Canvas& canvas) override;
    [[nodiscard]] ::core::tui::EventResult key(::core::tui::KeyEvent const& key) override;
    void activate() override;

private:
    std::string _label;
    ui::Action _onClick;
};

/// Checkbox: `[x] label`; Enter, Space or a click flips it and reports the new state.
class CheckboxImpl final : public TuiWidget<ui::CheckboxWidget> {
public:
    explicit CheckboxImpl(Context& context) : TuiWidget{context} { adopt(makeView(*this)); }
    void setLabel(std::string_view label) override;
    void setChecked(bool checked) override;
    void setOnToggle(std::function<void(bool)> onToggle) override;
    [[nodiscard]] ::core::tui::Size naturalSize() const override;
    [[nodiscard]] bool wantsFocus() const override { return true; }
    [[nodiscard]] std::string probeText() const override { return _label; }
    void paint(::core::tui::Canvas& canvas) override;
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
/// next spinner frame; the frontend advances it on a timer while `Context::activeBusy` counts any active Busy.
class BusyImpl final : public TuiWidget<ui::BusyWidget> {
public:
    explicit BusyImpl(Context& context) : TuiWidget{context} { adopt(makeView(*this)); }
    /// @brief Stops counting as active.
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

/// Slider: `[====|----] value`. Left and Right move by one step, Home and End to the ends, each reporting the new
/// value once it changed; no setter reports.
class SliderImpl final : public TuiWidget<ui::SliderWidget> {
public:
    explicit SliderImpl(Context& context) : TuiWidget{context} { adopt(makeView(*this)); }
    void setRange(std::int64_t minimum, std::int64_t maximum, std::int64_t step) override;
    void setValue(std::int64_t value) override;
    void setOnChange(std::function<void(std::int64_t)> onChange) override;
    [[nodiscard]] ::core::tui::Size naturalSize() const override { return {.width = 24, .height = 1}; }
    [[nodiscard]] bool wantsFocus() const override { return true; }
    [[nodiscard]] std::string probeText() const override;
    void paint(::core::tui::Canvas& canvas) override;
    [[nodiscard]] ::core::tui::EventResult key(::core::tui::KeyEvent const& key) override;

private:
    /// The value one step above the current one, or the maximum when that is closer.
    [[nodiscard]] std::int64_t stepAbove() const;
    /// The value one step below the current one, or the minimum when that is closer.
    [[nodiscard]] std::int64_t stepBelow() const;
    /// The track cell of the current value, among @p cells.
    [[nodiscard]] int knobCell(int cells) const;
    void moveTo(std::int64_t value);

    std::int64_t _minimum = 0;
    std::int64_t _maximum = 100;
    std::int64_t _step = 1;
    std::int64_t _value = 0;
    std::function<void(std::int64_t)> _onChange;
};

}  // namespace morph::tui::detail

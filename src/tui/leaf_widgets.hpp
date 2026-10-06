// SPDX-License-Identifier: Apache-2.0

#pragma once
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

}  // namespace morph::tui::detail

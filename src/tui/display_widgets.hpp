// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <core/tui/Canvas.hpp>
#include <core/tui/InputEvent.hpp>
#include <core/tui/Rect.hpp>
#include <cstddef>
#include <morph/ui/backend.hpp>
#include <morph/ui/view.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "tui/widget.hpp"

namespace morph::tui::detail {

/// The theme's style for @p tone: info, success, warning, error, or normal text for a neutral tone.
[[nodiscard]] ::core::tui::Style const& toneStyle(::core::tui::Theme const& theme, ui::Tone tone);

/// Banner: one row in its tone, `text`, then `[ action ]` when it has an action label and `[x]` when dismissible.
/// It takes the focus when it has either. Enter or Space, or a click on the action, calls `onAction`; Esc or Delete,
/// or a click on `[x]`, calls `onDismiss` and leaves the banner shown.
class BannerImpl final : public TuiWidget<ui::BannerWidget> {
public:
    explicit BannerImpl(Context& context) : TuiWidget{context} { adopt(makeView(*this)); }
    void setTone(ui::Tone tone) override;
    void setText(std::string_view text) override;
    void setActionLabel(std::string_view label) override;
    void setOnAction(ui::Action onAction) override { _onAction = std::move(onAction); }
    void setDismissible(bool dismissible) override;
    void setOnDismiss(ui::Action onDismiss) override { _onDismiss = std::move(onDismiss); }
    [[nodiscard]] ::core::tui::Size naturalSize() const override;
    [[nodiscard]] bool wantsFocus() const override { return !_actionLabel.empty() || _dismissible; }
    [[nodiscard]] std::string probeText() const override { return _text; }
    void paint(::core::tui::Canvas& canvas) override;
    [[nodiscard]] ::core::tui::EventResult key(::core::tui::KeyEvent const& key) override;
    void activate() override;
    void click(::core::tui::Point cell) override;
    /// Calls `onDismiss` when the banner is dismissible, as Esc does.
    void dismiss();

private:
    /// Where the action and the dismissal show on the row: their first column, or none.
    struct Controls {
        std::optional<int> action;
        std::optional<int> dismissal;
    };
    [[nodiscard]] Controls controls() const;

    ui::Tone _tone = ui::Tone::Neutral;
    std::string _text;
    std::string _actionLabel;
    bool _dismissible = false;
    ui::Action _onAction;
    ui::Action _onDismiss;
};

/// Toast: one row in its tone, in flow where it is mounted. The terminal has no floating/overlay concept and no
/// per-widget timer to dismiss it on, so `show` only replaces the shown text and tone; an application wanting it
/// to disappear again calls `setVisible(false)` itself.
class ToastImpl final : public TuiWidget<ui::ToastWidget> {
public:
    explicit ToastImpl(Context& context) : TuiWidget{context} { adopt(makeView(*this)); }
    void show(std::string_view text, ui::Tone tone) override;
    [[nodiscard]] ::core::tui::Size naturalSize() const override;
    [[nodiscard]] std::string probeText() const override { return _text; }
    void paint(::core::tui::Canvas& canvas) override;

private:
    ui::Tone _tone = ui::Tone::Neutral;
    std::string _text;
};

/// Badge: `[text]` in its tone. A terminal has no icon map: a framework icon is drawn as its glyph before the text,
/// and any other is kept and not drawn.
class BadgeImpl final : public TuiWidget<ui::BadgeWidget> {
public:
    explicit BadgeImpl(Context& context) : TuiWidget{context} { adopt(makeView(*this)); }
    void setTone(ui::Tone tone) override;
    void setText(std::string_view text) override;
    void setIcon(std::string_view icon) override {
        _icon = std::string{icon};
        refresh();
    }
    [[nodiscard]] ::core::tui::Size naturalSize() const override;
    [[nodiscard]] std::string probeText() const override { return _text; }
    void paint(::core::tui::Canvas& canvas) override;

private:
    [[nodiscard]] std::string caption() const;
    ui::Tone _tone = ui::Tone::Neutral;
    std::string _text;
    std::string _icon;
};

/// Progress: a bar of `#` over `.` and the percentage, or `<=>` for an indeterminate one, then the label.
class ProgressImpl final : public TuiWidget<ui::ProgressWidget> {
public:
    explicit ProgressImpl(Context& context) : TuiWidget{context} { adopt(makeView(*this)); }
    void setValue(std::optional<double> value) override;
    void setLabel(std::string_view label) override;
    [[nodiscard]] ::core::tui::Size naturalSize() const override;
    [[nodiscard]] std::string probeText() const override { return _label; }
    void paint(::core::tui::Canvas& canvas) override;

private:
    std::optional<double> _value;
    std::string _label;
};

/// Steps: one row per step, its state as `[ ]` pending, `[>]` active, `[x]` done or `[!]` failed, the current one
/// accented.
class StepsImpl final : public TuiWidget<ui::StepsWidget> {
public:
    explicit StepsImpl(Context& context) : TuiWidget{context} { adopt(makeView(*this)); }
    void setSteps(std::vector<ui::Step> const& steps) override;
    void setCurrent(std::optional<std::size_t> index) override;
    [[nodiscard]] ::core::tui::Size naturalSize() const override;
    /// The current step's label.
    [[nodiscard]] std::string probeText() const override;
    void paint(::core::tui::Canvas& canvas) override;

private:
    std::vector<ui::Step> _steps;
    std::optional<std::size_t> _current;
};

/// Key-value: one row per pair, the captions padded to the widest, then the values.
class KeyValueImpl final : public TuiWidget<ui::KeyValueWidget> {
public:
    explicit KeyValueImpl(Context& context) : TuiWidget{context} { adopt(makeView(*this)); }
    void setItems(std::vector<ui::KeyValueItem> const& items) override;
    [[nodiscard]] ::core::tui::Size naturalSize() const override;
    [[nodiscard]] std::string probeText() const override;
    void paint(::core::tui::Canvas& canvas) override;

private:
    [[nodiscard]] int captionWidth() const;

    std::vector<ui::KeyValueItem> _items;
};

/// Empty state: the title in bold, the explanation muted, then `[ action ]` when it has an action label, which takes
/// the focus; Enter or Space, or a click on it, calls `onAction`. The icon is kept and not drawn.
class EmptyStateImpl final : public TuiWidget<ui::EmptyStateWidget> {
public:
    explicit EmptyStateImpl(Context& context) : TuiWidget{context} { adopt(makeView(*this)); }
    void setTitle(std::string_view title) override;
    void setText(std::string_view text) override;
    void setIcon(std::string_view icon) override { _icon = std::string{icon}; }
    void setActionLabel(std::string_view label) override;
    void setOnAction(ui::Action onAction) override { _onAction = std::move(onAction); }
    [[nodiscard]] ::core::tui::Size naturalSize() const override;
    [[nodiscard]] bool wantsFocus() const override { return !_actionLabel.empty(); }
    [[nodiscard]] std::string probeText() const override { return _title; }
    void paint(::core::tui::Canvas& canvas) override;
    [[nodiscard]] ::core::tui::EventResult key(::core::tui::KeyEvent const& key) override;
    void activate() override;
    void click(::core::tui::Point cell) override;

private:
    [[nodiscard]] int actionRow() const;

    std::string _title;
    std::string _text;
    std::string _icon;
    std::string _actionLabel;
    ui::Action _onAction;
};

}  // namespace morph::tui::detail

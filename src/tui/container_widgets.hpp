// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <core/tui/Canvas.hpp>
#include <core/tui/Component.hpp>
#include <core/tui/InputEvent.hpp>
#include <core/tui/Rect.hpp>
#include <cstddef>
#include <functional>
#include <morph/ui/backend.hpp>
#include <morph/ui/view.hpp>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "tui/context.hpp"
#include "tui/layout.hpp"
#include "tui/widget.hpp"

namespace morph::tui::detail {

/// Column or Row (and ForEach, and a Table row): children along one axis, with a gap.
class StackImpl final : public TuiContainer<ui::StackWidget> {
public:
    StackImpl(Context& context, ui::Axis axis);
    void setGap(int gap) override;
    [[nodiscard]] ui::Axis axis() const noexcept { return _axis; }
    [[nodiscard]] int gap() const noexcept { return _gap; }
    /// Leading shown children given no area: what a Scroll around this stack has scrolled past.
    void setSkip(std::size_t skip);
    [[nodiscard]] std::size_t skip() const noexcept { return _skip; }
    /// Fixed main-axis extents and their gap, for a Table row; an empty list returns the row to the solver.
    void setColumnLayout(std::vector<int> extents, int gap);
    [[nodiscard]] ::core::tui::Size naturalSize() const override;
    void paint(::core::tui::Canvas& canvas) override;

private:
    ui::Axis _axis;
    int _gap = 0;
    std::size_t _skip = 0;
    std::vector<int> _extents;
    int _extentGap = 0;
};

/// The slot a Switch case or a Tabs page mounts into: its children stacked vertically, no gap.
class SlotImpl final : public TuiContainer<ui::SlotWidget> {
public:
    explicit SlotImpl(Context& context);
    [[nodiscard]] ::core::tui::Size naturalSize() const override;
    void paint(::core::tui::Canvas& canvas) override;
};

/// Grid: equal columns, cells row-major, a cell spanning several columns. A hidden child leaves no cell: the cells
/// after it move up one place, as a null cell of the view tree leaves none.
class GridImpl final : public TuiContainer<ui::GridWidget> {
public:
    explicit GridImpl(Context& context);
    void setColumns(int columns) override;
    void setGap(int gap) override;
    /// Throws std::logic_error when @p child is not one of this grid's children.
    void setSpan(ui::Widget& child, int span) override;
    [[nodiscard]] ::core::tui::Size naturalSize() const override;
    void paint(::core::tui::Canvas& canvas) override;

protected:
    void childForgotten(WidgetBase& child) override;

private:
    [[nodiscard]] int spanOf(WidgetBase const* child) const;
    [[nodiscard]] std::vector<layout::GridCell> cellsOf(std::span<WidgetBase* const> shown) const;

    int _columns = 1;
    int _gap = 0;
    std::unordered_map<WidgetBase const*, int> _spans;
};

/// Panel: a box with a title around its children, inset by padding; a collapsible panel folds to its title line.
///
/// Collapsing hides the children, so they neither draw nor take input; the panel itself, focusable while
/// collapsible, toggles on Enter or Space while it holds the focus, and on a click on its title line.
class PanelImpl final : public TuiContainer<ui::PanelWidget> {
public:
    explicit PanelImpl(Context& context);
    void setTitle(std::string_view title) override;
    void setPadding(int padding) override;
    void setCollapsible(bool collapsible) override;
    void setCollapsed(bool collapsed) override;
    void setOnToggle(std::function<void(bool)> onToggle) override;
    [[nodiscard]] ::core::tui::Size naturalSize() const override;
    [[nodiscard]] bool wantsFocus() const override { return _collapsible; }
    [[nodiscard]] std::string probeText() const override { return _title; }
    [[nodiscard]] bool letsChildrenAct() const override { return !_collapsed; }
    void paint(::core::tui::Canvas& canvas) override;
    [[nodiscard]] ::core::tui::EventResult key(::core::tui::KeyEvent const& key) override;
    void activate() override;
    void click(::core::tui::Point cell) override;

protected:
    void childAttached(WidgetBase& child) override;

private:
    [[nodiscard]] std::string heading() const;
    void syncChildren();

    std::string _title;
    int _padding = 0;
    bool _collapsible = false;
    bool _collapsed = false;
    std::function<void(bool)> _onToggle;
};

/// Scroll: shows as much of its content as fits. When the content is a stack along the scroll's axis, whole
/// children scroll out of view, the focused one is always kept in view, the view never ends short of the content
/// while there is more of it, and the wheel moves one child. Other content does not scroll.
///
/// A child scrolled out of view is given no area, so it neither draws nor takes a click.
class ScrollImpl final : public TuiContainer<ui::ScrollWidget> {
public:
    ScrollImpl(Context& context, ui::Axis axis);
    [[nodiscard]] ::core::tui::Size naturalSize() const override;
    void paint(::core::tui::Canvas& canvas) override;
    [[nodiscard]] bool wheel(int delta) override;

private:
    [[nodiscard]] StackImpl* scrolledStack() const;
    [[nodiscard]] std::size_t skipFor(StackImpl const& stack, int viewport) const;

    ui::Axis _axis;
    std::size_t _skip = 0;
};

}  // namespace morph::tui::detail

// SPDX-License-Identifier: Apache-2.0

#include "tui/container_widgets.hpp"

#include <algorithm>
#include <core/tui/Box.hpp>
#include <core/tui/Screen.hpp>
#include <core/tui/Theme.hpp>
#include <cstdint>
#include <iterator>
#include <memory>
#include <numeric>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <utility>

namespace morph::tui::detail {

StackImpl::StackImpl(Context& context, ui::Axis axis) : TuiContainer{context}, _axis{axis} { adopt(makeView(*this)); }

void StackImpl::setGap(int gap) {
    _gap = gap;
    refresh();
}

void StackImpl::setSkip(std::size_t skip) { _skip = skip; }

void StackImpl::setColumnLayout(std::vector<int> extents, int gap) {
    _extents = std::move(extents);
    _extentGap = gap;
}

::core::tui::Size StackImpl::naturalSize() const {
    if (_extents.empty()) {
        return stackNaturalSize(children(), _axis, _gap);
    }
    auto const natural = stackNaturalSize(children(), _axis, 0);
    int const columns =
        std::accumulate(_extents.begin(), _extents.end(), 0) + (_extentGap * (static_cast<int>(_extents.size()) - 1));
    return _axis == ui::Axis::Horizontal ? ::core::tui::Size{.width = columns, .height = natural.height}
                                         : ::core::tui::Size{.width = natural.width, .height = columns};
}

void StackImpl::paint(::core::tui::Canvas& canvas) {
    arrangeStack(
        children(), canvas.area(),
        StackSpec{.axis = _axis, .gap = _extents.empty() ? _gap : _extentGap, .skip = _skip, .extents = _extents});
}

SlotImpl::SlotImpl(Context& context) : TuiContainer{context} { adopt(makeView(*this)); }

::core::tui::Size SlotImpl::naturalSize() const { return stackNaturalSize(children(), ui::Axis::Vertical, 0); }

void SlotImpl::paint(::core::tui::Canvas& canvas) {
    arrangeStack(children(), canvas.area(), StackSpec{.axis = ui::Axis::Vertical});
}

namespace {

/// The main-axis extents a scrolled stack's shown children ask for, and what a run of them covers.
class StackExtents {
public:
    StackExtents(std::span<WidgetBase* const> items, StackImpl const& stack) : _gap{std::max(0, stack.gap())} {
        bool const vertical = stack.axis() == ui::Axis::Vertical;
        _prefix.reserve(items.size() + 1);
        _prefix.push_back(0);
        for (auto const* item : items) {
            _prefix.push_back(_prefix.back() + requested(*item, vertical));
        }
    }

    /// The extent of items [first, last], the gaps between them included.
    [[nodiscard]] std::int64_t covered(std::size_t first, std::size_t last) const {
        return _prefix.at(last + 1) - _prefix.at(first) +
               (std::int64_t{_gap} * static_cast<std::int64_t>(last - first));
    }

private:
    /// What the stack's solver gives @p item when there is room: a Fixed amount, else the natural extent.
    static std::int64_t requested(WidgetBase const& item, bool vertical) {
        auto const sizing = vertical ? item.layout().height : item.layout().width;
        if (sizing.kind == ui::Sizing::Kind::Fixed) {
            return std::max(0, sizing.amount);
        }
        auto const natural = item.naturalSize();
        return std::max(0, vertical ? natural.height : natural.width);
    }

    int _gap;
    std::vector<std::int64_t> _prefix;
};

/// Which of @p items holds the screen's focus, itself or below it.
std::optional<std::size_t> indexHoldingFocus(std::span<WidgetBase* const> items, ::core::tui::Screen const& screen) {
    auto const* const focused = screen.focusedComponent();
    auto const found =
        std::ranges::find_if(items, [focused](WidgetBase const* item) { return isWithin(focused, item->view()); });
    if (found == items.end()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(std::distance(items.begin(), found));
}

}  // namespace

GridImpl::GridImpl(Context& context) : TuiContainer{context} { adopt(makeView(*this)); }

void GridImpl::setColumns(int columns) {
    _columns = columns;
    refresh();
}

void GridImpl::setGap(int gap) {
    _gap = gap;
    refresh();
}

void GridImpl::setSpan(ui::Widget& child, int span) {
    auto& cell = WidgetBase::of(child);
    if (cell.container() != this) {
        throw std::logic_error{"morph::tui: setSpan of a widget that is not this grid's child"};
    }
    _spans.insert_or_assign(&cell, span);
    refresh();
}

void GridImpl::childForgotten(WidgetBase& child) { _spans.erase(&child); }

int GridImpl::spanOf(WidgetBase const* child) const {
    auto const found = _spans.find(child);
    return found == _spans.end() ? 1 : found->second;
}

std::vector<layout::GridCell> GridImpl::cellsOf(std::span<WidgetBase* const> shown) const {
    std::vector<layout::GridCell> cells;
    cells.reserve(shown.size());
    for (auto const* child : shown) {
        cells.push_back(layout::GridCell{
            .span = spanOf(child), .naturalHeight = child->naturalSize().height, .height = child->layout().height});
    }
    return cells;
}

::core::tui::Size GridImpl::naturalSize() const {
    auto const shown = shownChildren();
    if (shown.empty()) {
        return {.width = 0, .height = 0};
    }
    int const columns = std::max(1, _columns);
    int const gap = std::max(0, _gap);
    int unit = 0;
    for (auto const* child : shown) {
        int const span = std::clamp(spanOf(child), 1, columns);
        int const width = std::max(0, child->naturalSize().width - (gap * (span - 1)));
        unit = std::max(unit, (width + span - 1) / span);
    }
    int const width = (unit * columns) + (gap * (columns - 1));
    auto const plan = layout::planGrid(columns, cellsOf(shown), layout::Track{.length = width, .gap = gap});
    int const height = std::accumulate(plan.rowHeights.begin(), plan.rowHeights.end(), 0) +
                       (gap * (static_cast<int>(plan.rowHeights.size()) - 1));
    return {.width = width, .height = height};
}

void GridImpl::paint(::core::tui::Canvas& canvas) {
    auto const shown = shownChildren();
    auto const plan = layout::planGrid(_columns, cellsOf(shown), layout::Track{.length = canvas.width(), .gap = _gap});
    for (auto const [child, slot] : std::views::zip(shown, plan.slots)) {
        auto area = layout::slotArea(plan, slot);
        area.width = layout::crossExtent(child->layout().width, area.width);
        area.height = layout::crossExtent(child->layout().height, area.height);
        child->view().setArea(area);
    }
}

PanelImpl::PanelImpl(Context& context) : TuiContainer{context} { adopt(makeView(*this)); }

void PanelImpl::setTitle(std::string_view title) {
    _title = std::string{title};
    refresh();
}

void PanelImpl::setPadding(int padding) {
    _padding = std::max(0, padding);
    refresh();
}

void PanelImpl::setCollapsible(bool collapsible) {
    _collapsible = collapsible;
    refresh();
}

void PanelImpl::setCollapsed(bool collapsed) {
    _collapsed = collapsed;
    syncChildren();
}

void PanelImpl::setOnToggle(std::function<void(bool)> onToggle) { _onToggle = std::move(onToggle); }

std::string PanelImpl::heading() const {
    if (!_collapsible) {
        return _title;
    }
    return (_collapsed ? "▸ " : "▾ ") + _title;
}

void PanelImpl::childAttached(WidgetBase& child) { child.setStructuralVisible(!_collapsed); }

void PanelImpl::syncChildren() {
    for (auto* child : children()) {
        child->setStructuralVisible(!_collapsed);
    }
    refresh();
}

::core::tui::Size PanelImpl::naturalSize() const {
    if (_collapsed) {
        return {.width = displayWidth(heading()), .height = 1};
    }
    auto const inner = stackNaturalSize(children(), ui::Axis::Vertical, 0);
    return {.width = std::max(inner.width + (2 * _padding) + 2, displayWidth(heading()) + 4),
            .height = inner.height + (2 * _padding) + 2};
}

// The title is written into a canvas of its own between the corners rather than handed to drawBox, which shortens
// a title by bytes and so can cut a UTF-8 sequence, such as the arrow, in two.
void PanelImpl::paint(::core::tui::Canvas& canvas) {
    auto const& theme = canvas.theme();
    auto const& style = hasFocus() ? theme.buttonFocused : theme.textNormal;
    if (_collapsed) {
        canvas.putString(0, 0, heading(), style);
        return;
    }
    canvas.drawBox(canvas.area(), ::core::tui::BorderStyle::Single, style);
    auto title = canvas.subcanvas({.x = 2, .y = 0, .width = std::max(0, canvas.width() - 4), .height = 1});
    title.putString(0, 0, heading(), style);
    auto const inside = ::core::tui::Rect{
        .x = 1, .y = 1, .width = std::max(0, canvas.width() - 2), .height = std::max(0, canvas.height() - 2)};
    arrangeStack(children(), layout::pad(inside, _padding), StackSpec{.axis = ui::Axis::Vertical});
}

// A key a focused child ignored bubbles up to this view; only the panel's own focus toggles it.
::core::tui::EventResult PanelImpl::key(::core::tui::KeyEvent const& key) {
    if (!_collapsible || !hasFocus() || !isActivation(key)) {
        return ::core::tui::EventResult::Ignored;
    }
    activate();
    return ::core::tui::EventResult::Handled;
}

// Optimistic, like a checkbox: the panel folds at once, and a binding that disagrees sets it back through
// setCollapsed.
void PanelImpl::activate() {
    if (!_collapsible || !actionable()) {
        return;
    }
    _collapsed = !_collapsed;
    syncChildren();
    bool const collapsed = _collapsed;
    auto const handler = _onToggle;
    if (handler) {
        handler(collapsed);
    }
}

// A press anywhere in the panel that no child takes bubbles here, so only the title line toggles.
void PanelImpl::click(::core::tui::Point cell) {
    if (cell.y == 0) {
        activate();
    }
}

ScrollImpl::ScrollImpl(Context& context, ui::Axis axis) : TuiContainer{context}, _axis{axis} {
    adopt(makeView(*this));
}

::core::tui::Size ScrollImpl::naturalSize() const { return stackNaturalSize(children(), _axis, 0); }

StackImpl* ScrollImpl::scrolledStack() const {
    auto const shown = shownChildren();
    if (shown.empty()) {
        return nullptr;
    }
    auto* const stack = dynamic_cast<StackImpl*>(shown.front());
    return stack != nullptr && stack->axis() == _axis ? stack : nullptr;
}

// First the view is kept full: it moves back while the children from one earlier to the last still fit. Then the
// focused child is brought into it, at the top when it lies before the view and at the bottom when after.
std::size_t ScrollImpl::skipFor(StackImpl const& stack, int viewport) const {
    auto const items = stack.shownChildren();
    if (items.empty()) {
        return 0;
    }
    StackExtents const extents{items, stack};
    std::size_t const last = items.size() - 1;
    std::size_t skip = std::min(_skip, last);
    while (skip > 0 && extents.covered(skip - 1, last) <= viewport) {
        --skip;
    }
    if (auto const focused = indexHoldingFocus(items, *context().screen)) {
        skip = std::min(skip, *focused);
        while (skip < *focused && extents.covered(skip, *focused) > viewport) {
            ++skip;
        }
    }
    return skip;
}

void ScrollImpl::paint(::core::tui::Canvas& canvas) {
    if (auto* const stack = scrolledStack()) {
        _skip = skipFor(*stack, _axis == ui::Axis::Vertical ? canvas.height() : canvas.width());
        stack->setSkip(_skip);
    }
    arrangeStack(children(), canvas.area(), StackSpec{.axis = _axis});
}

bool ScrollImpl::wheel(int delta) {
    if (scrolledStack() == nullptr) {
        return false;
    }
    if (delta >= 0) {
        ++_skip;
    } else if (_skip > 0) {
        --_skip;
    }
    refresh();
    return true;
}

}  // namespace morph::tui::detail

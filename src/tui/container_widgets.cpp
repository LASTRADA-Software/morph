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

void StackImpl::setGap(int gap) {
    _gap = gap;
    refresh();
}

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

std::vector<::core::tui::Rect> StackImpl::childAreas(::core::tui::Size size) const {
    return stackAreas(children(), {.x = 0, .y = 0, .width = size.width, .height = size.height},
                      StackSpec{.axis = _axis, .gap = _extents.empty() ? _gap : _extentGap, .extents = _extents});
}

void StackImpl::paint(::core::tui::Canvas& canvas) { placeChildren(childAreas(canvas.size())); }

::core::tui::Size SlotImpl::naturalSize() const { return stackNaturalSize(children(), ui::Axis::Vertical, 0); }

std::vector<::core::tui::Rect> SlotImpl::childAreas(::core::tui::Size size) const {
    return stackAreas(children(), {.x = 0, .y = 0, .width = size.width, .height = size.height},
                      StackSpec{.axis = ui::Axis::Vertical});
}

void SlotImpl::paint(::core::tui::Canvas& canvas) { placeChildren(childAreas(canvas.size())); }

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
        int const width = std::max(0, requestedSize(*child).width - (gap * (span - 1)));
        unit = std::max(unit, (width + span - 1) / span);
    }
    int const width = (unit * columns) + (gap * (columns - 1));
    auto const plan = layout::planGrid(columns, cellsOf(shown), layout::Track{.length = width, .gap = gap});
    int const height = std::accumulate(plan.rowHeights.begin(), plan.rowHeights.end(), 0) +
                       (gap * (static_cast<int>(plan.rowHeights.size()) - 1));
    return {.width = width, .height = height};
}

std::vector<::core::tui::Rect> GridImpl::childAreas(::core::tui::Size size) const {
    auto const shown = shownChildren();
    auto const plan = layout::planGrid(_columns, cellsOf(shown), layout::Track{.length = size.width, .gap = _gap});
    std::vector<::core::tui::Rect> areas;
    areas.reserve(children().size());
    auto slot = plan.slots.begin();
    for (auto const* child : children()) {
        if (!child->shown() || slot == plan.slots.end()) {
            areas.emplace_back();
            continue;
        }
        auto area = layout::slotArea(plan, *slot);
        area.width = layout::crossExtent(child->layout().width, area.width);
        area.height = layout::crossExtent(child->layout().height, area.height);
        areas.push_back(area);
        ++slot;
    }
    return areas;
}

void GridImpl::paint(::core::tui::Canvas& canvas) { placeChildren(childAreas(canvas.size())); }

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

std::vector<::core::tui::Rect> PanelImpl::childAreas(::core::tui::Size size) const {
    if (_collapsed) {
        return std::vector<::core::tui::Rect>(children().size());
    }
    auto const inside = ::core::tui::Rect{
        .x = 1, .y = 1, .width = std::max(0, size.width - 2), .height = std::max(0, size.height - 2)};
    return stackAreas(children(), layout::pad(inside, _padding), StackSpec{.axis = ui::Axis::Vertical});
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
    placeChildren(childAreas(canvas.size()));
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

::core::tui::Size ScrollImpl::naturalSize() const { return stackNaturalSize(children(), _axis, 0); }

std::vector<::core::tui::Rect> ScrollImpl::contentAreas(::core::tui::Size viewport) const {
    auto const natural = naturalSize();
    auto content = ::core::tui::Rect{.x = 0, .y = 0, .width = viewport.width, .height = viewport.height};
    if (_axis == ui::Axis::Vertical) {
        content.height = std::max(viewport.height, natural.height);
    } else {
        content.width = std::max(viewport.width, natural.width);
    }
    return stackAreas(children(), content, StackSpec{.axis = _axis});
}

std::vector<::core::tui::Rect> ScrollImpl::childAreas(::core::tui::Size size) const {
    auto areas = contentAreas(size);
    bool const vertical = _axis == ui::Axis::Vertical;
    for (auto& area : areas) {
        if (!area.empty()) {
            area = area.offset(vertical ? 0 : -_offset, vertical ? -_offset : 0);
        }
    }
    return areas;
}

namespace {

/// Where @p child lies among @p areas, the areas of @p container's children; empty when the container does not say.
::core::tui::Rect areaOf(ContainerBase const& container, std::span<::core::tui::Rect const> areas,
                         WidgetBase const* child) {
    auto const children = container.children();
    if (areas.size() != children.size()) {
        return {};
    }
    auto const found = std::ranges::find(children, child);
    if (found == children.end()) {
        return {};
    }
    return areas.subspan(static_cast<std::size_t>(std::distance(children.begin(), found))).front();
}

}  // namespace

// Walks down from this scroll's child to the focused widget, each container saying where the next one lies inside
// it. Where one does not say, or puts the next out of its own area, the last area known is the answer.
std::optional<::core::tui::Rect> ScrollImpl::focusedArea(::core::tui::Size viewport) const {
    std::vector<WidgetBase const*> chain;
    for (auto const* widget = context().ownerOf(context().screen->focusedComponent()); widget != nullptr;
         widget = widget->container()) {
        chain.push_back(widget);
        if (widget->container() == this) {
            break;
        }
    }
    if (chain.empty() || chain.back()->container() != this) {
        return std::nullopt;
    }
    auto area = areaOf(*this, contentAreas(viewport), chain.back());
    for (auto const [inner, outer] :
         std::views::zip(chain | std::views::reverse | std::views::drop(1), chain | std::views::reverse)) {
        auto const* const container = dynamic_cast<ContainerBase const*>(outer);
        if (container == nullptr || area.empty()) {
            break;
        }
        auto const within = areaOf(*container, container->childAreas(area.size()), inner);
        auto const placed = within.offset(area.x, area.y).intersect(area);
        if (within.empty() || placed.empty()) {
            break;
        }
        area = placed;
    }
    if (area.empty()) {
        return std::nullopt;
    }
    return area;
}

void ScrollImpl::followFocus(::core::tui::Size viewport) {
    auto const* const focused = context().screen->focusedComponent();
    bool const inside = focused != nullptr && focused != &view() && isWithin(focused, view());
    if (!inside) {
        _followed = nullptr;
        return;
    }
    if (focused == _followed) {
        return;
    }
    _followed = focused;
    auto const area = focusedArea(viewport);
    if (!area) {
        return;
    }
    bool const vertical = _axis == ui::Axis::Vertical;
    int const first = vertical ? area->y : area->x;
    int const last = first + (vertical ? area->height : area->width);
    if (last > _offset + _viewport) {
        _offset = last - _viewport;
    }
    _offset = std::min(_offset, first);
}

void ScrollImpl::paint(::core::tui::Canvas& canvas) {
    auto const viewport = canvas.size();
    bool const vertical = _axis == ui::Axis::Vertical;
    auto const natural = naturalSize();
    _viewport = vertical ? viewport.height : viewport.width;
    _extent = std::max(_viewport, vertical ? natural.height : natural.width);
    followFocus(viewport);
    _offset = std::clamp(_offset, 0, _extent - _viewport);
    placeChildren(childAreas(viewport));
}

bool ScrollImpl::scrollBy(int cells) {
    int const target = std::clamp(_offset + cells, 0, std::max(0, _extent - _viewport));
    if (target == _offset) {
        return false;
    }
    _offset = target;
    refresh();
    return true;
}

bool ScrollImpl::wheel(int delta) { return scrollBy(delta); }

}  // namespace morph::tui::detail

// SPDX-License-Identifier: Apache-2.0

#include "tui/container_widgets.hpp"

#include <algorithm>
#include <core/tui/Box.hpp>
#include <core/tui/InputEvent.hpp>
#include <core/tui/KeyCode.hpp>
#include <core/tui/Modifier.hpp>
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
#include <variant>
#include <vector>

#include "tui/drag.hpp"

namespace morph::tui::detail {

void StackImpl::setGap(int gap) {
    _gap = layout::stepCells(gap, _axis);
    refresh();
}

void StackImpl::setAlign(ui::Align align) {
    _align = align;
    refresh();
}

void StackImpl::setJustify(ui::Justify justify) {
    _justify = justify;
    refresh();
}

void StackImpl::setColumnLayout(std::vector<int> extents, int gap, std::vector<CellPlace> places) {
    _extents = std::move(extents);
    _extentGap = gap;
    _places = std::move(places);
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
                      StackSpec{.axis = _axis,
                                .gap = _extents.empty() ? _gap : _extentGap,
                                .extents = _extents,
                                .places = _places,
                                // A table row's cells are placed by their columns, whatever the row's own
                                // alignment: a cell takes none.
                                .align = _extents.empty() ? _align : ui::Align::Center,
                                .justify = _justify});
}

void StackImpl::paint(::core::tui::Canvas& canvas) { placeChildren(childAreas(canvas.size())); }

::core::tui::Size ScrollImpl::naturalSize() const { return stackNaturalSize(children(), _scroller.axis(), 0); }

std::vector<::core::tui::Rect> ScrollImpl::childAreas(::core::tui::Size size) const {
    return _scroller.childAreas(*this, size);
}

bool ScrollImpl::wantsFocus() const { return ScrollViewport::wantsFocus(*this); }

void ScrollImpl::paint(::core::tui::Canvas& canvas) { _scroller.paint(*this, canvas); }

::core::tui::EventResult ScrollImpl::key(::core::tui::KeyEvent const& key) { return _scroller.key(*this, key); }

bool ScrollImpl::wheel(int delta) { return _scroller.wheel(*this, delta); }

::core::tui::Size SlotImpl::naturalSize() const { return stackNaturalSize(children(), ui::Axis::Vertical, 0); }

std::vector<::core::tui::Rect> SlotImpl::childAreas(::core::tui::Size size) const {
    if (scrolls()) {
        return _scroller.childAreas(*this, size);
    }
    return stackAreas(children(), {.x = 0, .y = 0, .width = size.width, .height = size.height},
                      StackSpec{.axis = ui::Axis::Vertical});
}

bool SlotImpl::wantsFocus() const { return scrolls() && ScrollViewport::wantsFocus(*this); }

void SlotImpl::paint(::core::tui::Canvas& canvas) {
    if (scrolls()) {
        _scroller.paint(*this, canvas);
        return;
    }
    placeChildren(childAreas(canvas.size()));
}

::core::tui::EventResult SlotImpl::key(::core::tui::KeyEvent const& key) {
    return scrolls() ? _scroller.key(*this, key) : ::core::tui::EventResult::Ignored;
}

bool SlotImpl::wheel(int delta) { return scrolls() && _scroller.wheel(*this, delta); }

void SlotImpl::setScrollable(bool scrollable) {
    if (scrollable != _scrollable) {
        _scrollable = scrollable;
        refresh();
    }
}

// A child that stretches down fills the slot, so only content-height children scroll.
bool SlotImpl::scrolls() const {
    return _scrollable && std::ranges::none_of(children(), [](WidgetBase const* child) {
               return child->shown() && child->layout().height.kind == ui::Sizing::Kind::Stretch;
           });
}

void GridImpl::setColumns(int columns) {
    _columns = columns;
    refresh();
}

void GridImpl::setTracks(std::vector<ui::GridTrack> const& tracks) {
    _tracks = tracks;
    refresh();
}

void GridImpl::setGap(int gap) {
    _columnGap = layout::stepCells(gap, ui::Axis::Horizontal);
    _rowGap = layout::stepCells(gap, ui::Axis::Vertical);
    refresh();
}

void GridImpl::setAlign(ui::Align align) {
    _align = align;
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
        cells.push_back(
            layout::GridCell{.span = spanOf(child),
                             .naturalHeight = child->naturalSize().height,
                             .height = child->layout().height,
                             .bounds = layout::boundsOf(child->layout().minHeight, child->layout().maxHeight)});
    }
    return cells;
}

std::vector<layout::Item> GridImpl::trackItems(std::span<WidgetBase* const> shown) const {
    std::vector<layout::Item> items;
    items.reserve(_tracks.size());
    for (ui::GridTrack const& track : _tracks) {
        items.push_back(layout::Item{.sizing = track.width, .natural = 0});
    }
    // Where each cell lands depends only on the spans, so a plan of no width says which track each one is in.
    auto const slots = layout::planGrid(items, cellsOf(shown), layout::Track{}, 0).slots;
    for (auto const [child, slot] : std::views::zip(shown, slots)) {
        if (slot.span == 1) {
            auto& item = items.at(static_cast<std::size_t>(slot.column));
            item.natural = std::max(item.natural, requestedSize(*child).width);
        }
    }
    return items;
}

layout::GridPlan GridImpl::planOf(std::span<WidgetBase* const> shown, int width) const {
    layout::Track const track{.length = width, .gap = _columnGap};
    return _tracks.empty() ? layout::planGrid(_columns, cellsOf(shown), track, _rowGap)
                           : layout::planGrid(trackItems(shown), cellsOf(shown), track, _rowGap);
}

::core::tui::Size GridImpl::naturalSize() const {
    auto const shown = shownChildren();
    if (shown.empty()) {
        return {.width = 0, .height = 0};
    }
    if (!_tracks.empty()) {
        // Each track at its natural width: a fixed one its amount, a content or stretch one its widest cell.
        auto const items = trackItems(shown);
        int width = _columnGap * (static_cast<int>(items.size()) - 1);
        for (layout::Item const& item : items) {
            width += item.sizing.kind == ui::Sizing::Kind::Fixed ? std::max(0, item.sizing.amount) : item.natural;
        }
        auto const plan = planOf(shown, width);
        int const height = std::accumulate(plan.rowHeights.begin(), plan.rowHeights.end(), 0) +
                           (_rowGap * (static_cast<int>(plan.rowHeights.size()) - 1));
        return {.width = width, .height = height};
    }
    int const columns = std::max(1, _columns);
    int unit = 0;
    for (auto const* child : shown) {
        int const span = std::clamp(spanOf(child), 1, columns);
        int const width = std::max(0, requestedSize(*child).width - (_columnGap * (span - 1)));
        unit = std::max(unit, (width + span - 1) / span);
    }
    int const width = (unit * columns) + (_columnGap * (columns - 1));
    auto const plan =
        layout::planGrid(columns, cellsOf(shown), layout::Track{.length = width, .gap = _columnGap}, _rowGap);
    int const height = std::accumulate(plan.rowHeights.begin(), plan.rowHeights.end(), 0) +
                       (_rowGap * (static_cast<int>(plan.rowHeights.size()) - 1));
    return {.width = width, .height = height};
}

std::vector<::core::tui::Rect> GridImpl::childAreas(::core::tui::Size size) const {
    auto const shown = shownChildren();
    auto const plan = planOf(shown, size.width);
    std::vector<::core::tui::Rect> areas;
    areas.reserve(children().size());
    auto slot = plan.slots.begin();
    for (auto const* child : children()) {
        if (!child->shown() || slot == plan.slots.end()) {
            areas.emplace_back();
            continue;
        }
        auto area = layout::slotArea(plan, *slot);
        // Across a column of the grid the cell fills its width; in its row, the grid's or the cell's own
        // alignment places it.
        ui::LayoutHints const& hints = child->layout();
        area.width = layout::crossExtent(hints.width, area.width, layout::boundsOf(hints.minWidth, hints.maxWidth));
        // A track places a cell drawn at its natural width at its start, centre or end; a spanning cell by its
        // first track.
        if (!_tracks.empty() && child->placedAcrossColumn()) {
            ui::Align const across = _tracks.at(static_cast<std::size_t>(slot->column)).align;
            if (across == ui::Align::Center || across == ui::Align::End) {
                int const natural = std::min(requestedSize(*child).width, area.width);
                area.x += alignedOffset(across, natural, area.width);
                area.width = natural;
            }
        }
        ui::Align const placed = hints.alignSelf.value_or(_align);
        int const height = alignedExtent(placed, hints.height, requestedSize(*child).height, area.height,
                                         layout::boundsOf(hints.minHeight, hints.maxHeight));
        area.y += alignedOffset(placed, height, area.height);
        area.height = height;
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

void PanelImpl::setHeader(ui::Widget& header) {
    _header = &WidgetBase::of(header);
    syncChildren();
}

void PanelImpl::setPadding(int padding) {
    _paddingRows = layout::stepCells(padding, ui::Axis::Vertical);
    _paddingColumns = layout::stepCells(padding, ui::Axis::Horizontal);
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

void PanelImpl::childAttached(WidgetBase& child) { child.setStructuralVisible(!_collapsed || &child == _header); }

void PanelImpl::childForgotten(WidgetBase& child) {
    if (&child == _header) {
        _header = nullptr;
    }
}

std::vector<WidgetBase*> PanelImpl::body() const {
    std::vector<WidgetBase*> out;
    for (auto* child : children()) {
        if (child != _header) {
            out.push_back(child);
        }
    }
    return out;
}

void PanelImpl::syncChildren() {
    for (auto* child : children()) {
        child->setStructuralVisible(!_collapsed || child == _header);
    }
    auto& shared = context();
    auto const* const focused = shared.screen->focusedComponent();
    refresh();
    // The focused widget is inside, so out of reach by now: losing the focus commits nothing and runs no handler,
    // unless it is the header, which stays reachable while collapsed.
    if (_collapsed && focused != nullptr && focused != &view() && isWithin(focused, view()) &&
        (_header == nullptr || !isWithin(focused, _header->view()))) {
        static_cast<void>(focusWidget(shared, focusable() ? this : nullptr));
    }
    // Last: a dialog inside that shows again takes the focus, and a field losing it may run a handler that destroys
    // this panel.
    followDialogs(shared);
}

::core::tui::Size PanelImpl::naturalSize() const {
    int const headerHeight = (_header != nullptr && _header->shown()) ? requestedSize(*_header).height : 0;
    int const headerWidth = (_header != nullptr && _header->shown()) ? requestedSize(*_header).width : 0;
    if (_collapsed) {
        return {.width = std::max(displayWidth(heading()), headerWidth + 2), .height = 1 + headerHeight};
    }
    auto const inner = stackNaturalSize(body(), ui::Axis::Vertical, 0);
    return {.width = std::max({inner.width + (2 * _paddingColumns) + 2, displayWidth(heading()) + 4, headerWidth + 2}),
            .height = inner.height + (2 * _paddingRows) + 2 + headerHeight};
}

std::vector<::core::tui::Rect> PanelImpl::childAreas(::core::tui::Size size) const {
    std::vector<::core::tui::Rect> areas(children().size());
    int const headerHeight = (_header != nullptr && _header->shown()) ? requestedSize(*_header).height : 0;
    std::vector<::core::tui::Rect> bodyAreas;
    if (!_collapsed) {
        auto const inside = ::core::tui::Rect{.x = 1,
                                              .y = 1 + headerHeight,
                                              .width = std::max(0, size.width - 2),
                                              .height = std::max(0, size.height - 2 - headerHeight)};
        bodyAreas = stackAreas(body(), layout::pad(inside, _paddingRows, _paddingColumns),
                               StackSpec{.axis = ui::Axis::Vertical});
    }
    std::size_t next = 0;
    std::size_t index = 0;
    for (auto const* const child : children()) {
        if (child == _header) {
            areas.at(index) = {.x = 1, .y = 1, .width = std::max(0, size.width - 2), .height = headerHeight};
        } else if (next < bodyAreas.size()) {
            areas.at(index) = bodyAreas.at(next++);
        }
        ++index;
    }
    return areas;
}

// The title is written into a canvas of its own between the corners rather than handed to drawBox, which shortens
// a title by bytes and so can cut a UTF-8 sequence, such as the arrow, in two.
void PanelImpl::paint(::core::tui::Canvas& canvas) {
    auto const& theme = canvas.theme();
    auto const& style = hasFocus() ? theme.buttonFocused : theme.textNormal;
    if (_collapsed) {
        canvas.putString(0, 0, heading(), style);
        placeChildren(childAreas(canvas.size()));
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

// A press anywhere in the panel that no child takes bubbles here, so only the title line focuses and toggles.
void PanelImpl::click(::core::tui::Point cell) {
    // Taking the focus may run a handler (a field elsewhere committing) that destroys this panel.
    if (cell.y == 0 && focusable() && focusWidget(context(), this)) {
        activate();
    }
}

std::vector<::core::tui::Rect> ScrollViewport::contentAreas(ContainerBase const& owner,
                                                            ::core::tui::Size viewport) const {
    auto const natural = stackNaturalSize(owner.children(), _axis, 0);
    auto content = ::core::tui::Rect{.x = 0, .y = 0, .width = viewport.width, .height = viewport.height};
    if (_axis == ui::Axis::Vertical) {
        content.height = std::max(viewport.height, natural.height);
    } else {
        content.width = std::max(viewport.width, natural.width);
    }
    return stackAreas(owner.children(), content, StackSpec{.axis = _axis});
}

std::vector<::core::tui::Rect> ScrollViewport::childAreas(ContainerBase const& owner, ::core::tui::Size size) const {
    auto areas = contentAreas(owner, size);
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
std::optional<::core::tui::Rect> ScrollViewport::focusedArea(ContainerBase const& owner,
                                                             ::core::tui::Size viewport) const {
    std::vector<WidgetBase const*> chain;
    for (auto const* widget = owner.context().ownerOf(owner.context().screen->focusedComponent()); widget != nullptr;
         widget = widget->container()) {
        chain.push_back(widget);
        if (widget->container() == &owner) {
            break;
        }
    }
    if (chain.empty() || chain.back()->container() != &owner) {
        return std::nullopt;
    }
    auto area = areaOf(owner, contentAreas(owner, viewport), chain.back());
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

// Moving the view where the user asked does not count as a change: the focused widget's place in the content and
// the view's size are what is compared.
void ScrollViewport::followFocus(ContainerBase const& owner, ::core::tui::Size viewport) {
    auto const* const focused = owner.context().screen->focusedComponent();
    bool const inside = focused != nullptr && focused != &owner.view() && isWithin(focused, owner.view());
    if (!inside) {
        _followed = {};
        return;
    }
    auto const area = focusedArea(owner, viewport);
    Followed const now{.view = focused, .area = area.value_or(::core::tui::Rect{}), .viewport = viewport};
    if (now == _followed) {
        return;
    }
    _followed = now;
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

void ScrollViewport::paint(ContainerBase const& owner, ::core::tui::Canvas& canvas) {
    auto const viewport = canvas.size();
    _viewportSize = viewport;
    bool const vertical = _axis == ui::Axis::Vertical;
    auto const natural = stackNaturalSize(owner.children(), _axis, 0);
    _viewport = vertical ? viewport.height : viewport.width;
    _extent = std::max(_viewport, vertical ? natural.height : natural.width);
    followFocus(owner, viewport);
    _offset = std::clamp(_offset, 0, _extent - _viewport);
    owner.placeChildren(childAreas(owner, viewport));
}

// A move the user asked for counts as having followed the focus as it is now, so the next frame does not undo it
// when the focus moved since the last one.
bool ScrollViewport::scrollBy(ContainerBase const& owner, int cells) {
    int const target = std::clamp(_offset + cells, 0, std::max(0, _extent - _viewport));
    if (target == _offset) {
        return false;
    }
    _offset = target;
    auto const* const focused = owner.context().screen->focusedComponent();
    _followed = {.view = focused,
                 .area = focusedArea(owner, _viewportSize).value_or(::core::tui::Rect{}),
                 .viewport = _viewportSize};
    owner.refresh();
    return true;
}

bool ScrollViewport::wheel(ContainerBase const& owner, int delta) { return scrollBy(owner, delta); }

// Each widget below says whether it wants the focus; a scroll among them asks its own content in turn.
bool ScrollViewport::wantsFocus(ContainerBase const& owner) {
    std::vector<WidgetBase const*> pending{owner.children().begin(), owner.children().end()};
    while (!pending.empty()) {
        auto const* const widget = pending.back();
        pending.pop_back();
        if (!widget->actionable()) {
            continue;
        }
        if (widget->wantsFocus()) {
            return false;
        }
        if (auto const* const container = dynamic_cast<ContainerBase const*>(widget)) {
            pending.insert(pending.end(), container->children().begin(), container->children().end());
        }
    }
    return true;
}

::core::tui::EventResult ScrollViewport::key(ContainerBase const& owner, ::core::tui::KeyEvent const& key) {
    using ::core::tui::KeyCode;
    if (::core::tui::withoutLockKeys(key.modifiers) != ::core::tui::Modifier::None) {
        return ::core::tui::EventResult::Ignored;
    }
    bool const vertical = _axis == ui::Axis::Vertical;
    int const page = std::max(1, _viewport - 1);
    int cells = 0;
    if (key.key == (vertical ? KeyCode::Up : KeyCode::Left)) {
        cells = -1;
    } else if (key.key == (vertical ? KeyCode::Down : KeyCode::Right)) {
        cells = 1;
    } else if (key.key == KeyCode::PageUp) {
        cells = -page;
    } else if (key.key == KeyCode::PageDown) {
        cells = page;
    } else if (key.key == KeyCode::Home) {
        cells = -_offset;
    } else if (key.key == KeyCode::End) {
        cells = _extent - _viewport - _offset;
    }
    return cells != 0 && scrollBy(owner, cells) ? ::core::tui::EventResult::Handled
                                                : ::core::tui::EventResult::Ignored;
}

namespace {

/// The widget @p component belongs to: the one whose view it is, the one whose open popup it is (a dropdown's
/// list), or the open dialog whose box it is; null for any other component.
WidgetBase* focusOwner(Context const& context, ::core::tui::Component const* component) {
    if (auto* const owner = context.ownerOf(component)) {
        return owner;
    }
    auto const popup = std::ranges::find_if(
        context.popups, [component](WidgetBase const* owner) { return owner->openPopup() == component; });
    if (popup != context.popups.end()) {
        return *popup;
    }
    auto const found = std::ranges::find_if(
        context.openDialogs, [component](ContainerBase* dialog) { return &dialog->host() == component; });
    return found == context.openDialogs.end() ? nullptr : *found;
}

/// Whether @p widget is @p outer or lies inside it.
bool isInside(WidgetBase const* widget, WidgetBase const& outer) {
    for (; widget != nullptr; widget = widget->container()) {
        if (widget == &outer) {
            return true;
        }
    }
    return false;
}

/// Hands the focus to @p target when it is still there and can take it, and is not behind the open dialog on top;
/// else to the first focusable widget of that dialog, or of the roots when none is open — nowhere only when nothing
/// can take it. A dialog as @p target, which stands for its box, never takes the focus itself: the focus goes into
/// the dialog on top, which is that one when it is still open and nothing lies over it.
void returnFocus(Context& context, WidgetBase* target) {
    auto* const dialog = activeDialog(context);
    bool const reachable = target != nullptr && (dialog == nullptr || isWithin(&target->view(), dialog->host()));
    if (reachable && focusWidget(context, target)) {
        return;
    }
    static_cast<void>(focusWidget(context, nullptr));
    moveFocus(context, Direction::Forward);
}

/// `enterDialog`, telling a dialog that the handler run by the old focus leaving opens which widget the focus left:
/// @p leaving, watched meanwhile.
void enterFrom(Context& context, WidgetBase* leaving) {
    WidgetBase* watched = leaving;
    context.watches.push_back(&watched);
    auto* const outer = std::exchange(context.focusLeaving, &watched);
    enterDialog(context);
    context.focusLeaving = outer;
    std::erase(context.watches, &watched);
}

}  // namespace

/// The dialog's box: an overlay that paints the border and the title and holds the children's views. Focusable, so
/// that Esc reaches it while the dialog holds nothing focusable.
class OverlayCore::Frame final : public ::core::tui::Component {
public:
    explicit Frame(OverlayCore& owner) : _owner{&owner} {}
    void render(::core::tui::Canvas& canvas) override { _owner->paintFrame(canvas); }
    [[nodiscard]] ::core::tui::Size preferredSize() const override { return _owner->frameSize(); }
    [[nodiscard]] bool visible() const noexcept override { return Component::visible() && _owner->showsFrame(); }
    [[nodiscard]] bool focusable() const override { return true; }
    /// onDismiss may destroy the dialog and this box with it, so nothing is touched after the call.
    [[nodiscard]] ::core::tui::EventResult onEvent(::core::tui::InputEvent const& event) override {
        return _owner->frameEvent(event);
    }

private:
    OverlayCore* _owner;
};

OverlayCore::OverlayCore(ContainerBase& self, std::optional<ui::Side> side)
    : _self{&self}, _side{side}, _frame{std::make_unique<Frame>(*this)} {}

OverlayCore::~OverlayCore() { closeFrame(); }

void OverlayCore::setOpen(bool open) {
    if (open) {
        openFrame();
    } else {
        closeFrame();
    }
}

void OverlayCore::setTitle(std::string_view title) {
    _title = std::string{title};
    _self->refresh();
}

void OverlayCore::setDescription(std::string_view description) {
    _description = std::string{description};
    _self->refresh();
}

void OverlayCore::setSheet(bool sheet) {
    _sheet = sheet;
    _self->refresh();
}

void OverlayCore::setDismissLabel(std::string_view label) {
    _dismissLabel = std::string{label};
    _self->refresh();
}

std::pair<int, int> OverlayCore::dismissCells() const { return {2, displayWidth(_dismissLabel) + 2}; }

::core::tui::Size FooterImpl::naturalSize() const { return stackNaturalSize(children(), ui::Axis::Horizontal, 1); }

std::vector<::core::tui::Rect> FooterImpl::childAreas(::core::tui::Size size) const {
    ::core::tui::Size const natural = naturalSize();
    int const width = std::min(size.width, natural.width);
    return stackAreas(children(), {.x = size.width - width, .y = 0, .width = width, .height = size.height},
                      StackSpec{.axis = ui::Axis::Horizontal, .gap = 1});
}

void FooterImpl::paint(::core::tui::Canvas& canvas) { placeChildren(childAreas(canvas.size())); }

ui::ContainerWidget& OverlayFooter::of(ContainerBase& overlay) {
    if (_footer == nullptr) {
        _footer = std::make_unique<FooterImpl>(overlay.context());
        overlay.attach(*_footer);
    }
    overlay.move(*_footer, overlay.children().size());
    return *_footer;
}

// A description takes the first row inside the frame and a rule the second, so the content starts two rows lower.
std::vector<::core::tui::Rect> OverlayCore::childAreas(::core::tui::Size size) const {
    int const head = _description.empty() ? 0 : 2;
    ::core::tui::Rect const inner{.x = 2,
                                  .y = 1 + head,
                                  .width = std::max(0, size.width - 4),
                                  .height = std::max(0, size.height - 2 - head)};
    std::span<WidgetBase* const> const children = _self->children();
    // A sheet's footer stands at the bottom of the box, under content that takes the rest of its height.
    bool const footerLast = _sheet && !children.empty() && children.back() == _footer && _footer->shown();
    if (!footerLast) {
        return stackAreas(children, inner, StackSpec{.axis = ui::Axis::Vertical});
    }
    int const footerHeight = std::min(inner.height, requestedSize(*_footer).height);
    std::vector<::core::tui::Rect> areas =
        stackAreas(children.first(children.size() - 1),
                   {.x = inner.x, .y = inner.y, .width = inner.width, .height = inner.height - footerHeight},
                   StackSpec{.axis = ui::Axis::Vertical});
    areas.push_back(
        {.x = inner.x, .y = inner.y + inner.height - footerHeight, .width = inner.width, .height = footerHeight});
    return areas;
}

bool OverlayCore::showsFrame() const {
    if (!_open || !_self->shown()) {
        return false;
    }
    WidgetBase const* inner = _self;
    for (ContainerBase const* outer = _self->container(); outer != nullptr;
         inner = outer, outer = outer->container()) {
        if (!outer->shown() || !outer->letsChildAct(*inner)) {
            return false;
        }
    }
    return true;
}

::core::tui::Size OverlayCore::frameSize() const {
    auto const inner = stackNaturalSize(_self->children(), ui::Axis::Vertical, 0);
    auto const area = _self->context().screen->viewportArea();
    if (_sheet && !_side.has_value()) {
        return {.width = area.width, .height = area.height};
    }
    int const width = std::max({inner.width + 4, displayWidth(_title) + 4, displayWidth(_description) + 4});
    int const head = _description.empty() ? 0 : 2;
    if (_side.has_value()) {
        return {.width = std::min(width, area.width), .height = area.height};
    }
    return {.width = std::min(width, area.width), .height = std::min(inner.height + 2 + head, area.height)};
}

// A dialog is centred; a drawer stands against its side, the whole height of the screen.
::core::tui::Point OverlayCore::framePosition() const {
    auto const& screen = *_self->context().screen;
    if (_sheet && !_side.has_value()) {
        auto const area = screen.viewportArea();
        return {.x = area.x, .y = area.y};
    }
    if (!_side.has_value()) {
        return centredIn(screen, frameSize());
    }
    auto const area = screen.viewportArea();
    int const left = *_side == ui::Side::Start ? area.x : area.x + area.width - frameSize().width;
    return {.x = left, .y = area.y};
}

// The title is written into a canvas of its own between the corners, as a panel's is: drawBox shortens a title by
// bytes and so can cut a UTF-8 sequence in two.
void OverlayCore::paintFrame(::core::tui::Canvas& canvas) {
    auto const& theme = canvas.theme();
    canvas.fill(canvas.area(), ' ', theme.dialogBackground);
    canvas.drawBox(canvas.area(), ::core::tui::BorderStyle::Single, theme.dialogBorder);
    auto title = canvas.subcanvas({.x = 2, .y = 0, .width = std::max(0, canvas.width() - 4), .height = 1});
    if (_sheet) {
        std::string const control = "[" + _dismissLabel + "]";
        title.putString(0, 0, control, theme.dialogTitle);
        title.putString(0, displayWidth(control) + 1, _title, theme.dialogTitle);
    } else {
        title.putString(0, 0, _title, theme.dialogTitle);
    }
    if (!_description.empty() && canvas.height() > 3) {
        // One row, clipped at the frame's width like any terminal text, then a rule across the frame's inside.
        auto line = canvas.subcanvas({.x = 2, .y = 1, .width = std::max(0, canvas.width() - 4), .height = 1});
        line.putString(0, 0, splitLines(_description).front(), theme.textMuted);
        for (int column = 1; column + 1 < canvas.width(); ++column) {
            canvas.putString(2, column, "─", theme.dialogBorder);
        }
    }
    _self->placeChildren(childAreas(canvas.size()));
}

// A pointer event here reached no widget inside: it goes nowhere, and in particular not to the tree behind. A
// press still ends a press whose release never came, as a press on a widget does.
::core::tui::EventResult OverlayCore::frameEvent(::core::tui::InputEvent const& event) {
    if (escapeEndsDrag(_self->context(), event)) {
        return ::core::tui::EventResult::Handled;
    }
    if (auto const* const mouse = std::get_if<::core::tui::MouseEvent>(&event)) {
        if (mouse->type == ::core::tui::MouseEvent::Type::Press) {
            _self->context().endPress();
            // core::tui made the event relative to the box, so this is the cell in it.
            auto const [first, width] = dismissCells();
            int const column = mouse->x - 1;
            bool const onDismiss = _sheet && mouse->y - 1 == 0 && column >= first && column < first + width;
            if (onDismiss && _open && _self->actionable()) {
                dismiss();
            }
        }
        return ::core::tui::EventResult::Handled;
    }
    auto const* const key = std::get_if<::core::tui::KeyEvent>(&event);
    if (key == nullptr || key->key != ::core::tui::KeyCode::Escape || !_open || !_self->actionable()) {
        return ::core::tui::EventResult::Ignored;
    }
    dismiss();
    return ::core::tui::EventResult::Handled;
}

// The dismissal closes the overlay first, as a toolkit's does: an application that keeps it open opens it again.
void OverlayCore::dismiss() {
    auto const handler = _onDismiss;
    closeFrame();
    if (handler) {
        handler();
    }
}

bool OverlayCore::holdsFocus() const {
    auto const* const focused = _self->context().screen->focusedComponent();
    return focused == nullptr || focused == _frame.get() || isInside(focusOwner(_self->context(), focused), *_self);
}

// Hidden and shown again, a box is drawn after every box shown before it; the dialogs keep the order of their boxes.
void OverlayCore::raiseNested() {
    auto& dialogs = _self->context().openDialogs;
    auto& screen = *_self->context().screen;
    std::vector<ContainerBase*> nested;
    std::ranges::copy_if(dialogs, std::back_inserter(nested),
                         [this](ContainerBase const* dialog) { return dialog != _self && isInside(dialog, *_self); });
    for (auto* dialog : nested) {
        std::erase(dialogs, dialog);
        dialogs.push_back(dialog);
        auto& frame = dialog->host();
        screen.hideOverlay(frame);
        screen.showOverlay(frame, dialog->overlay() != nullptr ? dialog->overlay()->framePosition()
                                                               : centredIn(screen, frame.preferredSize()));
    }
}

void OverlayCore::returnFocusTo(WidgetBase* widget) {
    forgetRestore();
    if (widget != nullptr) {
        _restore = widget;
        _self->context().watches.push_back(&_restore);
    }
}

void OverlayCore::forgetRestore() noexcept {
    std::erase(_self->context().watches, &_restore);
    _restore = nullptr;
}

// The box's bounds are emptied before it shows: it was last drawn wherever it was then, with whatever it held.
// A dialog opened inside a closed or hidden one takes no focus until its box shows. With the focus nowhere, it is
// on its way out of a widget for a dialog opening further out, whose handler opened this one: that widget is the
// one to go back to. The focus moves last: the old focus leaving runs a handler when it is a field with typed
// text, and that handler may destroy this dialog.
void OverlayCore::openFrame() {
    if (_open) {
        return;
    }
    auto& shared = _self->context();
    auto& screen = *shared.screen;
    _open = true;
    shared.openDialogs.push_back(_self);
    _frame->setScreenBounds({});
    forgetDrawnBounds(*_frame);
    screen.showOverlay(*_frame, framePosition());
    raiseNested();
    markFramed();
    countSpinners(shared);
    _self->refresh();
    if (!_framed) {
        return;
    }
    auto* owner = focusOwner(shared, screen.focusedComponent());
    if (owner == nullptr && shared.focusLeaving != nullptr) {
        owner = *shared.focusLeaving;
    }
    returnFocusTo(isInside(owner, *_self) ? nullptr : owner);
    enterFrom(shared, _restore);
}

void OverlayCore::markFramed() {
    for (auto* const entry : _self->context().openDialogs) {
        auto* const dialog = entry->overlay();
        if (dialog != nullptr && dialog != this && isInside(entry, *_self)) {
            dialog->_framed = dialog->_frame->visible();
        }
    }
    _framed = _open && _frame->visible();
}

// The widget the focus leaves is the one to go back to, unless the dialog knows one from when it opened. The focus
// moves last, as when opening.
void OverlayCore::regainFocus() {
    auto& shared = _self->context();
    if (activeDialog(shared) != _self) {
        return;
    }
    auto* const owner = focusOwner(shared, shared.screen->focusedComponent());
    if (owner != nullptr && isInside(owner, *_self)) {
        return;
    }
    if (_restore == nullptr) {
        returnFocusTo(owner);
    }
    enterFrom(shared, _restore);
}

// The dialog keeps the widget to go back to: closing hands the focus there again.
void OverlayCore::releaseFocus() {
    if (holdsFocus()) {
        returnFocus(_self->context(), _restore);
    }
}

// Each step may run a handler that destroys dialogs, so the list is searched afresh after each.
void OverlayCore::followFrames(Context& context) {
    for (;;) {
        OverlayCore* changed = nullptr;
        for (auto* const entry : context.openDialogs) {
            auto* const dialog = entry->overlay();
            if (dialog != nullptr && dialog->_framed != dialog->_frame->visible()) {
                changed = dialog;
                break;
            }
        }
        if (changed == nullptr) {
            return;
        }
        changed->_framed = !changed->_framed;
        if (changed->_framed) {
            changed->regainFocus();
        } else {
            changed->releaseFocus();
        }
    }
}

void followDialogs(Context& context) { OverlayCore::followFrames(context); }

// Nothing here runs a handler: a dropdown inside closes its list quietly, and a field inside commits nothing as the
// focus leaves it, the dialog being closed by then. The focus moves last all the same.
void OverlayCore::closeFrame() {
    if (!_open) {
        return;
    }
    auto& shared = _self->context();
    _open = false;
    markFramed();
    countSpinners(shared);
    closeUnreachablePopups(shared);
    bool const focusInside = holdsFocus();
    std::erase(shared.openDialogs, _self);
    shared.screen->hideOverlay(*_frame);
    _self->refresh();
    auto* const restore = _restore;
    forgetRestore();
    if (focusInside) {
        returnFocus(shared, restore);
    }
}

}  // namespace morph::tui::detail

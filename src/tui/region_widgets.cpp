// SPDX-License-Identifier: Apache-2.0

#include "tui/region_widgets.hpp"

#include <algorithm>
#include <core/tui/Theme.hpp>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <numeric>
#include <string>
#include <utility>

namespace morph::tui::detail {

using ::core::tui::EventResult;

std::string CollapsibleImpl::heading() const { return (_open ? "▾ " : "▸ ") + _title; }

void CollapsibleImpl::setTitle(std::string_view title) {
    _title = std::string{title};
    refresh();
}

void CollapsibleImpl::setHeader(ui::Widget& header) {
    _header = &WidgetBase::of(header);
    syncChildren();
}

void CollapsibleImpl::setOpen(bool open) {
    _open = open;
    syncChildren();
}

void CollapsibleImpl::childAttached(WidgetBase& child) { child.setStructuralVisible(_open || &child == _header); }

void CollapsibleImpl::childForgotten(WidgetBase& child) {
    if (&child == _header) {
        _header = nullptr;
    }
}

std::vector<WidgetBase*> CollapsibleImpl::body() const {
    std::vector<WidgetBase*> out;
    for (auto* child : children()) {
        if (child != _header) {
            out.push_back(child);
        }
    }
    return out;
}

// As a panel does: the focus inside a body that closes goes to the collapsible itself, without a handler running.
void CollapsibleImpl::syncChildren() {
    for (auto* child : children()) {
        child->setStructuralVisible(_open || child == _header);
    }
    auto& shared = context();
    auto const* const focused = shared.screen->focusedComponent();
    refresh();
    closeUnreachablePopups(shared);
    if (!_open && focused != nullptr && focused != &view() && isWithin(focused, view()) &&
        (_header == nullptr || !isWithin(focused, _header->view()))) {
        static_cast<void>(focusWidget(shared, focusable() ? this : nullptr));
    }
    followDialogs(shared);
}

::core::tui::Size CollapsibleImpl::naturalSize() const {
    int titleRow = displayWidth(heading());
    if (_header != nullptr && _header->shown()) {
        titleRow += 1 + requestedSize(*_header).width;
    }
    if (!_open) {
        return {.width = titleRow, .height = 1};
    }
    auto const inner = body();
    auto const content = stackNaturalSize(inner, ui::Axis::Vertical, 0);
    return {.width = std::max(titleRow, content.width), .height = 1 + content.height};
}

std::vector<::core::tui::Rect> CollapsibleImpl::childAreas(::core::tui::Size size) const {
    std::vector<::core::tui::Rect> areas(children().size());
    auto const inner = body();
    std::vector<::core::tui::Rect> bodyAreas;
    if (_open) {
        bodyAreas = stackAreas(inner, {.x = 0, .y = 1, .width = size.width, .height = std::max(0, size.height - 1)},
                               StackSpec{.axis = ui::Axis::Vertical});
    }
    std::size_t next = 0;
    std::size_t index = 0;
    for (auto const* const child : children()) {
        if (child == _header) {
            int const left = displayWidth(heading()) + 1;
            areas.at(index) = {.x = left, .y = 0, .width = std::max(0, size.width - left), .height = 1};
        } else if (next < bodyAreas.size()) {
            areas.at(index) = bodyAreas.at(next++);
        }
        ++index;
    }
    return areas;
}

void CollapsibleImpl::paint(::core::tui::Canvas& canvas) {
    auto const& theme = canvas.theme();
    canvas.putString(0, 0, heading(), hasFocus() ? theme.buttonFocused : theme.textBold);
    placeChildren(childAreas(canvas.size()));
}

// A key a focused child ignored bubbles up to this view; only the collapsible's own focus toggles it.
EventResult CollapsibleImpl::key(::core::tui::KeyEvent const& key) {
    if (!hasFocus() || !isActivation(key)) {
        return EventResult::Ignored;
    }
    activate();
    return EventResult::Handled;
}

// The user's request shows at once, and a binding that disagrees sets it back through setOpen.
void CollapsibleImpl::activate() {
    if (!actionable()) {
        return;
    }
    _open = !_open;
    syncChildren();
    bool const open = _open;
    auto const handler = _onToggle;
    if (handler) {
        handler(open);
    }
}

// Taking the focus may run a handler (a field elsewhere committing) that destroys this collapsible.
void CollapsibleImpl::click(::core::tui::Point cell) {
    if (cell.y == 0 && cell.x < displayWidth(heading()) && focusWidget(context(), this)) {
        activate();
    }
}

void SplitterImpl::setSizes(std::vector<int> const& sizes) {
    _sizes = sizes;
    refresh();
}

int SplitterImpl::along(::core::tui::Point cell) const noexcept {
    return _axis == ui::Axis::Horizontal ? cell.x : cell.y;
}

// The pane at @p index; a span has no checked access before C++26.
WidgetBase const* SplitterImpl::paneAt(std::size_t index) const {
    return *std::next(children().begin(), static_cast<std::ptrdiff_t>(index));
}

// The bounds of a pane along the splitter's axis.
layout::Bounds SplitterImpl::boundsOfPane(WidgetBase const& pane) const {
    ui::LayoutHints const& hints = pane.layout();
    return _axis == ui::Axis::Horizontal ? layout::boundsOf(hints.minWidth, hints.maxWidth)
                                         : layout::boundsOf(hints.minHeight, hints.maxHeight);
}

// The sizes asked for, each at least a cell and within its pane's bounds, the last pane taking what is left or giving
// up what does not fit. Without sizes, or with a count other than the panes', each pane is placed by its own sizing
// along a horizontal splitter (content counts as a stretch of weight 1, a percent as a share of the whole width, the
// handles included) and equally along a vertical one, each within its bounds and at least a cell.
std::vector<int> SplitterImpl::layoutExtents(int total) const {
    auto const panes = children().size();
    if (panes == 0) {
        return {};
    }
    int const handles = static_cast<int>(panes) - 1;
    return _sizes.size() != panes ? extentsBySizing(total) : extentsBySizes(std::max(0, total - handles));
}

// Each pane placed by its own sizing; @p total is the splitter's whole length, of which the handles take a cell each.
std::vector<int> SplitterImpl::extentsBySizing(int total) const {
    auto const panes = children().size();
    int const room = std::max(0, total - (static_cast<int>(panes) - 1));
    std::vector<layout::Item> items;
    items.reserve(panes);
    std::vector<std::size_t> sharing;
    bool stretches = false;
    for (auto const* pane : children()) {
        ui::Sizing sizing = ui::Sizing::stretch(1);
        if (_axis == ui::Axis::Horizontal) {
            sizing = pane->layout().width;
            if (sizing.kind == ui::Sizing::Kind::Content) {
                sizing = ui::Sizing::stretch(1);
            } else if (sizing.kind == ui::Sizing::Kind::Percent) {
                sizing = ui::Sizing::fixed(std::max(1, static_cast<int>(std::int64_t{total} * sizing.amount / 100)));
            }
        }
        stretches = stretches || sizing.kind == ui::Sizing::Kind::Stretch;
        if (sizing.kind == ui::Sizing::Kind::Stretch) {
            sharing.push_back(items.size());
        }
        layout::Bounds const bounds = boundsOfPane(*pane);
        items.push_back(layout::Item{.sizing = sizing, .minimum = bounds.minimum, .maximum = bounds.maximum});
    }
    std::vector<int> extents = layout::distribute(items, layout::Track{.length = room, .gap = 0});
    evenOutShares(items, sharing, extents);
    for (int& extent : extents) {
        extent = std::max(1, extent);
    }
    // With no pane to stretch, the last takes what the others leave.
    if (int const spare = room - std::accumulate(extents.begin(), extents.end(), 0); !stretches && spare > 0) {
        extents.back() += spare;
    }
    return extents;
}

// Panes that share by an equal weight, and that no bound has moved, differ by the cells a division leaves over; the
// last takes them all.
void SplitterImpl::evenOutShares(std::vector<layout::Item> const& items, std::vector<std::size_t> const& sharing,
                                 std::vector<int>& extents) {
    if (sharing.size() <= 1) {
        return;
    }
    int least = extents.at(sharing.front());
    int most = least;
    for (std::size_t const index : sharing) {
        least = std::min(least, extents.at(index));
        most = std::max(most, extents.at(index));
    }
    bool const equalWeights = std::ranges::all_of(sharing, [&](std::size_t index) {
        layout::Item const& item = items.at(index);
        return item.sizing.amount == items.at(sharing.front()).sizing.amount && item.minimum == 0 &&
               item.maximum == std::numeric_limits<int>::max();
    });
    if (equalWeights && most - least <= 1) {
        int extra = 0;
        for (std::size_t const index : sharing) {
            extra += extents.at(index) - least;
            extents.at(index) = least;
        }
        extents.at(sharing.back()) += extra;
    }
}

// The sizes asked for, each within its pane's bounds, the last pane taking what is left.
std::vector<int> SplitterImpl::extentsBySizes(int room) const {
    auto const panes = children().size();
    std::vector<int> extents(panes, 0);
    int used = 0;
    for (std::size_t index = 0; index + 1 < panes; ++index) {
        // Room is left for every pane after this one, a cell each.
        int const most = std::max(1, room - used - 1);
        layout::Bounds const bounds = boundsOfPane(*paneAt(index));
        int const low = std::min(bounds.minimum, most);
        extents.at(index) =
            std::clamp(std::clamp(_sizes.at(index), 1, most), low, std::max(low, std::min(bounds.maximum, most)));
        used += extents.at(index);
    }
    extents.back() = std::max(0, room - used);
    return extents;
}

::core::tui::Size SplitterImpl::naturalSize() const {
    auto const content = summedNaturalSize(children(), _axis, 1);
    if (_sizes.size() != children().size() || _sizes.empty()) {
        return content;
    }
    int const along = std::accumulate(_sizes.begin(), _sizes.end(), 0) + static_cast<int>(_sizes.size()) - 1;
    return _axis == ui::Axis::Horizontal ? ::core::tui::Size{.width = along, .height = content.height}
                                         : ::core::tui::Size{.width = content.width, .height = along};
}

std::vector<::core::tui::Rect> SplitterImpl::childAreas(::core::tui::Size size) const {
    bool const across = _axis == ui::Axis::Horizontal;
    _extents = layoutExtents(across ? size.width : size.height);
    std::vector<::core::tui::Rect> areas;
    areas.reserve(_extents.size());
    int start = 0;
    for (int const extent : _extents) {
        areas.push_back(across ? ::core::tui::Rect{.x = start, .y = 0, .width = extent, .height = size.height}
                               : ::core::tui::Rect{.x = 0, .y = start, .width = size.width, .height = extent});
        start += extent + 1;
    }
    return areas;
}

void SplitterImpl::paint(::core::tui::Canvas& canvas) {
    auto const areas = childAreas(canvas.size());
    auto const& style = _dragging ? canvas.theme().textAccent : canvas.theme().textMuted;
    bool const across = _axis == ui::Axis::Horizontal;
    for (std::size_t index = 0; index + 1 < areas.size(); ++index) {
        auto const& pane = areas.at(index);
        if (across) {
            for (int row = 0; row < canvas.height(); ++row) {
                canvas.putString(row, pane.x + pane.width, "│", style);
            }
        } else {
            for (int col = 0; col < canvas.width(); ++col) {
                canvas.putString(pane.y + pane.height, col, "─", style);
            }
        }
    }
    placeChildren(areas);
}

void SplitterImpl::pressAt(::core::tui::Point cell) {
    _dragging.reset();
    _moved = false;
    int start = 0;
    for (std::size_t index = 0; index + 1 < _extents.size(); ++index) {
        start += _extents.at(index);
        if (along(cell) == start) {
            _dragging = index;
            refresh();
            return;
        }
        ++start;
    }
}

// The handle follows the pointer, between the panes either side of it, each kept at least a cell.
void SplitterImpl::dragTo(::core::tui::Point cell) {
    if (!_dragging || *_dragging + 1 >= _extents.size()) {
        return;
    }
    auto const index = *_dragging;
    int before = 0;
    for (std::size_t pane = 0; pane < index; ++pane) {
        before += _extents.at(pane) + 1;
    }
    int const pair = _extents.at(index) + _extents.at(index + 1);
    // The handle stays where both panes keep their bounds; where those leave no room it does not move.
    layout::Bounds const lead = boundsOfPane(*paneAt(index));
    layout::Bounds const trail = boundsOfPane(*paneAt(index + 1));
    int const least =
        std::max({1, lead.minimum, trail.maximum == std::numeric_limits<int>::max() ? 1 : pair - trail.maximum});
    int const most = std::min({std::max(1, pair - 1), lead.maximum, pair - trail.minimum});
    if (least > most) {
        return;
    }
    int const first = std::clamp(along(cell) - before, least, most);
    if (first == _extents.at(index)) {
        return;
    }
    _sizes = _extents;
    _sizes.at(index) = first;
    _sizes.at(index + 1) = pair - first;
    _extents = _sizes;
    _moved = true;
    refresh();
}

bool SplitterImpl::pressEnded() {
    bool const moved = _dragging.has_value() && _moved;
    _dragging.reset();
    _moved = false;
    refresh();
    if (!moved) {
        return false;
    }
    auto sizes = _extents;
    auto const handler = _onResize;
    if (handler) {
        handler(std::move(sizes));
    }
    return true;
}

namespace {

[[nodiscard]] char lowered(char letter) noexcept {
    return letter >= 'A' && letter <= 'Z' ? static_cast<char>(letter - 'A' + 'a') : letter;
}

[[nodiscard]] bool endsWithIgnoringCase(std::string_view text, std::string_view suffix) noexcept {
    if (suffix.size() > text.size()) {
        return false;
    }
    auto const tail = text.substr(text.size() - suffix.size());
    return std::ranges::equal(tail, suffix, [](char left, char right) { return lowered(left) == lowered(right); });
}

[[nodiscard]] int hexValue(char digit) noexcept {
    if (digit >= '0' && digit <= '9') {
        return digit - '0';
    }
    char const lower = lowered(digit);
    return lower >= 'a' && lower <= 'f' ? lower - 'a' + 10 : -1;
}

/// @p uri's path, its `%XX` escapes decoded, or @p uri itself when it is no `file://` URI.
[[nodiscard]] std::string fromFileUri(std::string uri) {
    constexpr std::string_view kScheme = "file://";
    if (!uri.starts_with(kScheme)) {
        return uri;
    }
    std::string_view rest = std::string_view{uri}.substr(kScheme.size());
    if (auto const slash = rest.find('/'); slash != std::string_view::npos) {
        rest = rest.substr(slash);  // A host before the path (`file://localhost/...`) is not part of it.
    }
    std::string path;
    for (std::size_t index = 0; index < rest.size(); ++index) {
        if (rest.at(index) == '%' && index + 2 < rest.size()) {
            int const high = hexValue(rest.at(index + 1));
            int const low = hexValue(rest.at(index + 2));
            if (high >= 0 && low >= 0) {
                path += static_cast<char>((high * 16) + low);
                index += 2;
                continue;
            }
        }
        path += rest.at(index);
    }
    return path;
}

}  // namespace

std::vector<std::string> parsePastedPaths(std::string_view text) {
    std::vector<std::string> paths;
    std::string current;
    bool inPath = false;
    char quote = 0;
    for (std::size_t index = 0; index < text.size(); ++index) {
        char const letter = text.at(index);
        if (quote != 0) {
            if (letter == quote) {
                quote = 0;
            } else {
                current += letter;
            }
            continue;
        }
        if (letter == '\'' || letter == '"') {
            quote = letter;
            inPath = true;
        } else if (letter == '\\' && index + 1 < text.size()) {
            current += text.at(++index);
            inPath = true;
        } else if (letter == ' ' || letter == '\t' || letter == '\n' || letter == '\r') {
            if (inPath) {
                paths.push_back(fromFileUri(std::move(current)));
                current.clear();
                inPath = false;
            }
        } else {
            current += letter;
            inPath = true;
        }
    }
    if (inPath) {
        paths.push_back(fromFileUri(std::move(current)));
    }
    return paths;
}

::core::tui::Size DropZoneImpl::naturalSize() const {
    auto const inner = stackNaturalSize(children(), ui::Axis::Vertical, 0);
    int const width = children().empty() ? displayWidth("Drop files here") : inner.width;
    int const height = children().empty() ? 1 : inner.height;
    return {.width = width + 2, .height = height + 2};
}

std::vector<::core::tui::Rect> DropZoneImpl::childAreas(::core::tui::Size size) const {
    return stackAreas(children(),
                      {.x = 1, .y = 1, .width = std::max(0, size.width - 2), .height = std::max(0, size.height - 2)},
                      StackSpec{.axis = ui::Axis::Vertical});
}

void DropZoneImpl::paint(::core::tui::Canvas& canvas) {
    auto const& theme = canvas.theme();
    auto const& style = hasFocus() ? theme.textAccent : theme.textMuted;
    canvas.drawBox(canvas.area(), ::core::tui::BorderStyle::Dashed, style);
    if (children().empty() && canvas.height() > 2) {
        canvas.putString(1, 1, "Drop files here", theme.textMuted);
    }
    placeChildren(childAreas(canvas.size()));
}

EventResult DropZoneImpl::paste(std::string_view text) {
    if (!hasFocus()) {
        return EventResult::Ignored;
    }
    drop(parsePastedPaths(text));
    return EventResult::Handled;
}

bool DropZoneImpl::takes(std::string_view path) const {
    return _accept.empty() || std::ranges::any_of(_accept, [path](std::string const& extension) {
               return endsWithIgnoringCase(path, extension);
           });
}

void DropZoneImpl::drop(std::vector<std::string> paths) {
    if (paths.empty() || !actionable() || (paths.size() > 1 && !_multiple) ||
        !std::ranges::all_of(paths, [this](std::string const& path) { return takes(path); })) {
        return;
    }
    auto const handler = _onDrop;
    if (handler) {
        handler(std::move(paths));
    }
}

}  // namespace morph::tui::detail

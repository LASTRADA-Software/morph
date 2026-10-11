// SPDX-License-Identifier: Apache-2.0

#include "tui/region_widgets.hpp"

#include <algorithm>
#include <core/tui/Theme.hpp>
#include <cstddef>
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

// The sizes asked for, each at least a cell, the last pane taking what is left or giving up what does not fit; an
// equal share when no sizes, or a count other than the panes', were asked for.
std::vector<int> SplitterImpl::layoutExtents(int total) const {
    auto const panes = children().size();
    std::vector<int> extents(panes, 0);
    if (panes == 0) {
        return extents;
    }
    int const handles = static_cast<int>(panes) - 1;
    int const room = std::max(0, total - handles);
    if (_sizes.size() != panes) {
        int const share = room / static_cast<int>(panes);
        std::ranges::fill(extents, share);
        extents.back() = room - (share * (static_cast<int>(panes) - 1));
        return extents;
    }
    int used = 0;
    for (std::size_t index = 0; index + 1 < panes; ++index) {
        extents.at(index) = std::clamp(_sizes.at(index), 1, std::max(1, room - used - 1));
        used += extents.at(index);
    }
    extents.back() = std::max(0, room - used);
    return extents;
}

::core::tui::Size SplitterImpl::naturalSize() const {
    auto const content = stackNaturalSize(children(), _axis, 1);
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
    int const first = std::clamp(along(cell) - before, 1, std::max(1, pair - 1));
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

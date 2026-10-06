// SPDX-License-Identifier: Apache-2.0

#include "tui/container_widgets.hpp"

#include <memory>
#include <numeric>
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

}  // namespace morph::tui::detail

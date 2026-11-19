// SPDX-License-Identifier: Apache-2.0

#include "tui/context.hpp"

#include <algorithm>
#include <memory>
#include <vector>

#include "tui/drag.hpp"
#include "tui/table_widget.hpp"
#include "tui/widget.hpp"

namespace morph::tui::detail {

Context::Context(::core::tui::Screen& target) : screen{&target}, drag{std::make_unique<DragController>(*this)} {}

Context::~Context() = default;

WidgetBase* Context::ownerOf(::core::tui::Component const* view) const {
    auto const found = owners.find(view);
    return found == owners.end() ? nullptr : found->second;
}

void Context::forget(WidgetBase& widget) {
    std::erase(roots, &widget);
    std::erase(popups, &widget);
    std::erase(spotlights, &widget);
    if (spotlit == &widget) {
        spotlit = nullptr;
    }
    std::erase_if(spanTables,
                  [&widget](TableImpl const* table) { return static_cast<WidgetBase const*>(table) == &widget; });
    std::erase_if(windowTables,
                  [&widget](TableImpl const* table) { return static_cast<WidgetBase const*>(table) == &widget; });
    if (pressed == &widget) {
        pressed = nullptr;
    }
    for (auto** const watch : watches) {
        if (*watch == &widget) {
            *watch = nullptr;
        }
    }
    drag->forget(widget);
    targetsChanged();
}

bool Context::spotlightMisplaced() const {
    return std::ranges::any_of(spotlights, [](WidgetBase const* spotlight) { return spotlight->placementStale(); });
}

void Context::targetsChanged() const {
    for (WidgetBase* const spotlight : std::vector<WidgetBase*>{spotlights}) {
        spotlight->targetsChanged();
    }
}

void Context::endPress() {
    pressed = nullptr;
    drag->cancel();
}

}  // namespace morph::tui::detail

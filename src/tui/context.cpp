// SPDX-License-Identifier: Apache-2.0

#include "tui/context.hpp"

#include <memory>
#include <vector>

#include "tui/drag.hpp"

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
    if (pressed == &widget) {
        pressed = nullptr;
    }
    for (auto** const watch : watches) {
        if (*watch == &widget) {
            *watch = nullptr;
        }
    }
    drag->forget(widget);
}

void Context::endPress() {
    pressed = nullptr;
    drag->cancel();
}

}  // namespace morph::tui::detail

// SPDX-License-Identifier: Apache-2.0

#include "tui/context.hpp"

#include <vector>

namespace morph::tui::detail {

Context::Context(::core::tui::Screen& target) : screen{&target} {}

Context::~Context() = default;

WidgetBase* Context::ownerOf(::core::tui::Component const* view) const {
    auto const found = owners.find(view);
    return found == owners.end() ? nullptr : found->second;
}

void Context::forget(WidgetBase& widget) {
    std::erase(roots, &widget);
    if (pressed == &widget) {
        pressed = nullptr;
    }
}

}  // namespace morph::tui::detail

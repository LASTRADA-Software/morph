// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <morph/ui/backend.hpp>
#include <morph/ui/view.hpp>
#include <morph/util/datetime.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace morph::ui {

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the first row, the count, the total: the order setters take.
void TableWidget::setRealizedWindow(std::size_t first, std::size_t count, std::size_t total) {
    static_cast<void>(first);
    static_cast<void>(count);
    static_cast<void>(total);
}

std::unique_ptr<CustomWidget> IViewBackend::createCustom(ContainerWidget* parent, CustomRequest const& request) {
    static_cast<void>(parent);
    static_cast<void>(request);
    return nullptr;
}

std::unique_ptr<SpotlightWidget> IViewBackend::createSpotlight(ContainerWidget* parent) {
    static_cast<void>(parent);
    throw std::logic_error{"morph::ui: this backend draws no spotlight, so it runs no guided tour"};
}

// NOLINTNEXTLINE(modernize-use-string-view): the declaration returns std::string.
std::string widthClassOf(double units) {
    if (units < 80.0) {
        return "compact";
    }
    if (units < static_cast<double>(kExpandedWidthUnits)) {
        return "medium";
    }
    return "expanded";
}
}  // namespace morph::ui

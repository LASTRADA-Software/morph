// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <morph/ui/backend.hpp>
#include <morph/ui/view.hpp>
#include <morph/util/datetime.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace morph::ui {

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

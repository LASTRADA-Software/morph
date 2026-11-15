// SPDX-License-Identifier: Apache-2.0
#include <morph/ui/route_history.hpp>
#include <string>
#include <string_view>

namespace morph::ui {

std::string fragmentOf(std::string_view route) {
    std::string fragment{"#/"};
    fragment += route;
    return fragment;
}

std::string routeOfFragment(std::string_view fragment) {
    constexpr std::string_view prefix{"#/"};
    return fragment.starts_with(prefix) ? std::string{fragment.substr(prefix.size())} : std::string{};
}

}  // namespace morph::ui

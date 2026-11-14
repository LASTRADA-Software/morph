// SPDX-License-Identifier: Apache-2.0
#include "lims/billing/core/fault_hook.hpp"

namespace lims {

namespace {

/// @brief The installed hook; a function-local static, so it exists before any translation unit's first use.
[[nodiscard]] std::function<void(std::string_view, Lightweight::SqlConnection&)>& installed() {
    static std::function<void(std::string_view, Lightweight::SqlConnection&)> hook;
    return hook;
}

}  // namespace

void setFaultHook(std::function<void(std::string_view, Lightweight::SqlConnection&)> hook) {
    installed() = std::move(hook);
}

void faultPoint(std::string_view point, Lightweight::SqlConnection& connection) {
    if (auto const& hook = installed(); hook) {
        hook(point, connection);
    }
}

}  // namespace lims

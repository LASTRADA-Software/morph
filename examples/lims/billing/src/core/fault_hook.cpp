// SPDX-License-Identifier: Apache-2.0
#include "lims/billing/core/fault_hook.hpp"

namespace lims {

namespace {

/// @brief The installed hook; a function-local static, so it exists before any translation unit's first use.
[[nodiscard]] std::function<void(std::string_view)>& installed() {
    static std::function<void(std::string_view)> hook;
    return hook;
}

}  // namespace

void setFaultHook(std::function<void(std::string_view)> hook) { installed() = std::move(hook); }

void faultPoint(std::string_view point) {
    if (auto const& hook = installed(); hook) {
        hook(point);
    }
}

}  // namespace lims

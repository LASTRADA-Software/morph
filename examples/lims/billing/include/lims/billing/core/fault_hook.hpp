// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <functional>
#include <string_view>

/// @file
/// Named points inside billing's transactions where a test injects a failure. Empty in production, where
/// `faultPoint` does nothing.

namespace lims {

/// @brief Installs @p hook, called with each point's name; an empty hook removes it. Set by a test before it acts
///        and cleared after, never while another thread is in billing.
/// @param hook The hook; it fails the point by throwing.
void setFaultHook(std::function<void(std::string_view)> hook);

/// @brief Runs the installed hook for @p point, if any.
/// @param point The point's name, such as `run.afterInvoice`.
void faultPoint(std::string_view point);

}  // namespace lims

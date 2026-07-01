// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>

/// @file
/// DTOs shared by more than one model.

namespace bank::dto {

/// @brief Generic ok/message acknowledgement for commands without richer output.
struct CommandResult {
    bool ok = false;
    std::string message;
};

}  // namespace bank::dto

// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string_view>

/// @file
/// The server's own clients. Each acts through `InternalClient` as a principal in the reserved `system:` namespace,
/// which no sign-in yields, and holds `LimsRole::Service` at each company it acts for, assigned like any role
/// (`AssignRole`, the administration screen); the server grants itself nothing.

namespace lims::app {

/// @brief Relays lab's events into billing.
inline constexpr std::string_view kEventsPrincipal = "system:events";

/// @brief Runs the billing schedule.
inline constexpr std::string_view kSchedulePrincipal = "system:billing-schedule";

/// @brief Records billing's postings in the ledger.
inline constexpr std::string_view kPostingPrincipal = "system:posting";

}  // namespace lims::app

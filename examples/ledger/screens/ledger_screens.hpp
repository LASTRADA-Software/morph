// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <expected>
#include <memory>
#include <morph/ui/service.hpp>
#include <string_view>

/// @file
/// ledger's screen as a UI document (spec 4 §5): sign-in and a ledger, then tabs for its accounts and entries (store a
/// transaction, list a month, undo one), its budgets, its rules, and the monthly statement job, polled until it
/// finishes or fails. Served by the ledger server and linked in process by the generic client's local
/// mode. Toolkit-free: this target links morph and the rung's models only.

namespace ledger::screens {

/// @brief The application's id, as `ui-hello` names it.
inline constexpr std::string_view kApplicationId = "ledger";

/// @brief Sign-in and the ledger's tabs: accounts and entries, budgets, rules and the statement job.
inline constexpr std::string_view kLedger = "ledger.main";

/// @brief Prepares ledger to run in process: installs a token issuer with a development secret, which `Login` mints
///        its tokens with and a server installs at start-up.
void prepareLocal();

/// @brief The application's bundle: the ledger screen, registered and validated.
/// @return The bundle, or the refusal.
[[nodiscard]] std::expected<std::shared_ptr<morph::ui::ServedBundle const>, morph::ui::Refusal> bundle();

}  // namespace ledger::screens

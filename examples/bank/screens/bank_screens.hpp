// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <expected>
#include <memory>
#include <morph/ui/service.hpp>
#include <string_view>

/// @file
/// bank's screens as UI documents (spec 4 §5): sign-in, then accounts, move money, cards, payees and loans behind a
/// sidebar. Served by the bank server and linked in process by the generic client's local mode. Toolkit-free: this
/// target links morph and bank's models only.

namespace bank::screens {

/// @brief The application's id, as `ui-hello` names it.
inline constexpr std::string_view kApplicationId = "bank";

/// @brief Signs in, or registers and signs in.
inline constexpr std::string_view kLogin = "bank.login";

/// @brief The signed-in customer's accounts: cards, opening and closing, and an account's history.
inline constexpr std::string_view kAccounts = "bank.accounts";

/// @brief Deposits, withdrawals and transfers from a chosen account.
inline constexpr std::string_view kMove = "bank.move";

/// @brief The customer's cards: issue, freeze, unfreeze, cancel.
inline constexpr std::string_view kCards = "bank.cards";

/// @brief The customer's payees: add and remove.
inline constexpr std::string_view kPayees = "bank.payees";

/// @brief The customer's loans: apply, repay, and a loan's schedule.
inline constexpr std::string_view kLoans = "bank.loans";

/// @brief The application's bundle: every bank screen, registered and validated.
/// @return The bundle, or the refusal.
[[nodiscard]] std::expected<std::shared_ptr<morph::ui::ServedBundle const>, morph::ui::Refusal> bundle();

}  // namespace bank::screens

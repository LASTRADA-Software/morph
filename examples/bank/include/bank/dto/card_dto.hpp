// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>
#include <vector>

/// @file
/// Wire DTOs for the Card model.

namespace bank::dto {

/// @brief A payment card.
struct CardInfo {
    std::int64_t id = 0;
    std::string owner;
    std::int64_t accountId = 0;
    int kind = 0;  ///< bank::CardKind
    std::string panLast4;
    int status = 0;  ///< bank::CardStatus
    std::int64_t dailyLimitMinor = 0;
};

/// @brief Issue a new card linked to an account.
struct IssueCard {
    std::int64_t accountId = 0;
    int kind = 0;  ///< bank::CardKind
    std::int64_t dailyLimitMinor = 0;

    [[nodiscard]] bool validate() const { return accountId > 0 && kind >= 0 && kind <= 1; }
};

/// @brief Freeze a card (temporarily block use).
struct FreezeCard {
    std::int64_t id = 0;
};

/// @brief Unfreeze a previously frozen card.
struct UnfreezeCard {
    std::int64_t id = 0;
};

/// @brief Permanently cancel a card.
struct CancelCard {
    std::int64_t id = 0;
};

/// @brief Set a card's daily spend limit.
struct SetCardLimit {
    std::int64_t id = 0;
    std::int64_t dailyLimitMinor = 0;
};

/// @brief Change a card's PIN.
struct ChangePin {
    std::int64_t id = 0;
    std::string newPin;

    [[nodiscard]] bool validate() const { return newPin.size() == 4; }
};

/// @brief List the current owner's cards.
struct ListCards {
    std::string owner;  ///< empty => session principal
};

/// @brief Result of `ListCards`.
struct CardList {
    std::vector<CardInfo> cards;
};

}  // namespace bank::dto

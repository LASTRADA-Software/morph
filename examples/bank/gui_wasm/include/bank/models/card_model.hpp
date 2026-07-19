// SPDX-License-Identifier: Apache-2.0
#pragma once

// WASM shadow of include/bank/models/card_model.hpp (in-memory backend).

#include <morph/core/registry.hpp>
#include <morph/core/bridge.hpp>

#include "bank/dto/card_dto.hpp"
#include "bank/dto/common.hpp"

namespace bank {

/// @brief Issues and manages payment cards (in-memory).
class CardModel {
public:
    dto::CardInfo execute(const dto::IssueCard& action);
    dto::CommandResult execute(const dto::FreezeCard& action);
    dto::CommandResult execute(const dto::UnfreezeCard& action);
    dto::CommandResult execute(const dto::CancelCard& action);
    dto::CommandResult execute(const dto::SetCardLimit& action);
    dto::CommandResult execute(const dto::ChangePin& action);
    dto::CardList execute(const dto::ListCards& action);
};

}  // namespace bank

using bank::CardModel;
using bank::dto::CancelCard;
using bank::dto::ChangePin;
using bank::dto::FreezeCard;
using bank::dto::IssueCard;
using bank::dto::ListCards;
using bank::dto::SetCardLimit;
using bank::dto::UnfreezeCard;

BRIDGE_REGISTER_MODEL(CardModel, "CardModel")
BRIDGE_REGISTER_ACTION(CardModel, IssueCard, "IssueCard")
BRIDGE_REGISTER_ACTION(CardModel, FreezeCard, "FreezeCard")
BRIDGE_REGISTER_ACTION(CardModel, UnfreezeCard, "UnfreezeCard")
BRIDGE_REGISTER_ACTION(CardModel, CancelCard, "CancelCard")
BRIDGE_REGISTER_ACTION(CardModel, SetCardLimit, "SetCardLimit")
BRIDGE_REGISTER_ACTION(CardModel, ChangePin, "ChangePin")
BRIDGE_REGISTER_ACTION(CardModel, ListCards, "ListCards")

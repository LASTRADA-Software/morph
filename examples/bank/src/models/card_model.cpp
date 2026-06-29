// SPDX-License-Identifier: Apache-2.0

#include "bank/models/card_model.hpp"

#include <Lightweight/Lightweight.hpp>

#include <cstdint>
#include <format>
#include <random>
#include <string>
#include <string_view>

#include "bank/core/demo_hash.hpp"
#include "bank/core/errors.hpp"
#include "bank/core/principal.hpp"
#include "bank/core/types.hpp"
#include "bank/db/account_entity.hpp"
#include "bank/db/card_entity.hpp"
#include "bank/db/ledger_ops.hpp"

namespace bank {

namespace {

std::string randomLast4() {
    static thread_local std::mt19937 rng{std::random_device{}()};
    std::uniform_int_distribution<int> dist{0, 9999};
    return std::format("{:04}", dist(rng));
}

std::string hashPin(std::string_view pin) {
    return demoHash(std::string{pin} + ":pin");
}

dto::CardInfo toInfo(const db::CardRecord& rec) {
    return dto::CardInfo{
        .id = static_cast<std::int64_t>(rec.id.Value()),
        .owner = std::string{rec.owner.Value().str()},
        .accountId = rec.accountId.Value(),
        .kind = rec.kind.Value(),
        .panLast4 = std::string{rec.panLast4.Value().str()},
        .status = rec.status.Value(),
        .dailyLimitMinor = rec.dailyLimitMinor.Value(),
    };
}

}  // namespace

dto::CardInfo CardModel::execute(const dto::IssueCard& action) {
    if (!action.validate()) {
        throw ValidationError{"invalid card request"};
    }
    const std::string owner = sessionPrincipal();
    if (owner.empty()) {
        throw Unauthorized{"no session principal"};
    }
    // Cards may only be issued against an open account the caller owns.
    auto account = db::loadOwnedOpenAccount(mapper(), action.accountId, owner);

    db::CardRecord card;
    card.owner = Light::SqlAnsiString<64>{owner};
    card.accountId = action.accountId;
    card.kind = action.kind;
    card.panLast4 = Light::SqlAnsiString<4>{randomLast4()};
    card.status = static_cast<int>(CardStatus::Active);
    card.dailyLimitMinor = action.dailyLimitMinor;
    card.pinHash = Light::SqlAnsiString<16>{hashPin("0000")};
    mapper().Create(card);
    return toInfo(card);
}

namespace {

/// Loads a card the session owner owns, or throws.
db::CardRecord requireOwnedCard(Lightweight::DataMapper& mapper, std::int64_t cardId) {
    return db::loadOwned<db::CardRecord>(mapper, cardId, sessionPrincipal(), "card");
}

}  // namespace

dto::CommandResult CardModel::execute(const dto::FreezeCard& action) {
    auto card = requireOwnedCard(mapper(), action.id);
    card.status = static_cast<int>(CardStatus::Frozen);
    mapper().Update(card);
    return dto::CommandResult{.ok = true, .message = "card frozen"};
}

dto::CommandResult CardModel::execute(const dto::UnfreezeCard& action) {
    auto card = requireOwnedCard(mapper(), action.id);
    if (card.status.Value() == static_cast<int>(CardStatus::Cancelled)) {
        throw ConflictError{"cancelled cards cannot be reactivated"};
    }
    card.status = static_cast<int>(CardStatus::Active);
    mapper().Update(card);
    return dto::CommandResult{.ok = true, .message = "card active"};
}

dto::CommandResult CardModel::execute(const dto::CancelCard& action) {
    auto card = requireOwnedCard(mapper(), action.id);
    card.status = static_cast<int>(CardStatus::Cancelled);
    mapper().Update(card);
    return dto::CommandResult{.ok = true, .message = "card cancelled"};
}

dto::CommandResult CardModel::execute(const dto::SetCardLimit& action) {
    if (action.dailyLimitMinor < 0) {
        throw ValidationError{"limit must be non-negative"};
    }
    auto card = requireOwnedCard(mapper(), action.id);
    card.dailyLimitMinor = action.dailyLimitMinor;
    mapper().Update(card);
    return dto::CommandResult{.ok = true, .message = "limit updated"};
}

dto::CommandResult CardModel::execute(const dto::ChangePin& action) {
    if (!action.validate()) {
        throw ValidationError{"PIN must be exactly 4 digits"};
    }
    auto card = requireOwnedCard(mapper(), action.id);
    card.pinHash = Light::SqlAnsiString<16>{hashPin(action.newPin)};
    mapper().Update(card);
    return dto::CommandResult{.ok = true, .message = "PIN changed"};
}

dto::CardList CardModel::execute(const dto::ListCards& action) {
    const std::string owner = resolveOwner(action.owner);
    if (owner.empty()) {
        throw Unauthorized{"no session principal"};
    }
    auto rows = mapper()
                    .Query<db::CardRecord>()
                    .Where(Lightweight::FieldNameOf<&db::CardRecord::owner>, "=", owner)
                    .All();
    dto::CardList out;
    out.cards.reserve(rows.size());
    for (const auto& rec : rows) {
        out.cards.push_back(toInfo(rec));
    }
    return out;
}

}  // namespace bank

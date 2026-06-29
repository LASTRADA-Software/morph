// SPDX-License-Identifier: Apache-2.0

#include "bank/models/card_model.hpp"

#include <Lightweight/Lightweight.hpp>

#include <cstdint>
#include <format>
#include <functional>
#include <random>
#include <string>
#include <string_view>

#include "bank/core/errors.hpp"
#include "bank/core/principal.hpp"
#include "bank/core/types.hpp"
#include "bank/db/account_entity.hpp"
#include "bank/db/card_entity.hpp"

namespace bank {

namespace {

std::string randomLast4() {
    static thread_local std::mt19937 rng{std::random_device{}()};
    std::uniform_int_distribution<int> dist{0, 9999};
    return std::format("{:04}", dist(rng));
}

std::string hashPin(std::string_view pin) {
    return std::format("{:016x}", std::hash<std::string>{}(std::string{pin} + ":pin"));
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
    auto account = mapper().QuerySingle<db::AccountRecord>(static_cast<std::uint64_t>(action.accountId));
    if (!account.has_value()) {
        throw NotFound{"account not found"};
    }
    if (std::string{account->owner.Value().str()} != owner) {
        throw Unauthorized{"account belongs to a different owner"};
    }

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
    auto card = mapper.QuerySingle<db::CardRecord>(static_cast<std::uint64_t>(cardId));
    if (!card.has_value()) {
        throw NotFound{"card not found"};
    }
    if (std::string{card->owner.Value().str()} != sessionPrincipal()) {
        throw Unauthorized{"card belongs to a different owner"};
    }
    return *card;
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

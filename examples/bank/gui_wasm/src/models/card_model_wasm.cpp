// SPDX-License-Identifier: Apache-2.0
//
// In-memory implementation of CardModel for the WASM build.

#include <format>
#include <random>
#include <string>
#include <string_view>

#include "bank/core/demo_hash.hpp"
#include "bank/core/errors.hpp"
#include "bank/core/principal.hpp"
#include "bank/core/types.hpp"
#include "bank/models/card_model.hpp"
#include "bank/wasm/store.hpp"
#include "bank/wasm/store_ops.hpp"

namespace bank {

namespace {

std::string randomLast4() {
    static thread_local std::mt19937 rng{std::random_device{}()};
    std::uniform_int_distribution<int> dist{0, 9999};
    return std::format("{:04}", dist(rng));
}

std::string hashPin(std::string_view pin) { return demoHash(std::string{pin} + ":pin"); }

dto::CardInfo toInfo(const wasm::CardRow& rec, const std::string& owner) {
    return dto::CardInfo{
        .id = static_cast<std::int64_t>(rec.id),
        .owner = owner,
        .accountId = static_cast<std::int64_t>(rec.accountId),
        .kind = rec.kind,
        .panLast4 = rec.panLast4,
        .status = rec.status,
        .dailyLimitMinor = rec.dailyLimitMinor,
    };
}

/// Loads a card the current principal owns, or throws.
wasm::CardRow requireOwnedCard(wasm::Db& db, std::int64_t cardId) {
    const auto ownerId = wasm::requireUserId(db, sessionPrincipal());
    auto* card = db.cards.find(static_cast<std::uint64_t>(cardId));
    if (card == nullptr) {
        throw NotFound{"card not found"};
    }
    if (card->userId != ownerId) {
        throw Unauthorized{"card belongs to a different owner"};
    }
    return *card;
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
    auto& db = wasm::sharedDb();
    const auto ownerId = wasm::requireUserId(db, owner);
    auto account = wasm::loadOwnedOpenAccount(db, action.accountId, ownerId);

    wasm::CardRow card;
    card.accountId = account.id;
    card.userId = ownerId;
    card.kind = action.kind;
    card.panLast4 = randomLast4();
    card.status = static_cast<int>(CardStatus::Active);
    card.dailyLimitMinor = action.dailyLimitMinor;
    card.pinHash = hashPin("0000");
    card.id = db.cards.insert(card);
    return toInfo(card, owner);
}

dto::CommandResult CardModel::execute(const dto::FreezeCard& action) {
    auto& db = wasm::sharedDb();
    auto card = requireOwnedCard(db, action.id);
    card.status = static_cast<int>(CardStatus::Frozen);
    db.cards.update(card);
    return dto::CommandResult{.ok = true, .message = "card frozen"};
}

dto::CommandResult CardModel::execute(const dto::UnfreezeCard& action) {
    auto& db = wasm::sharedDb();
    auto card = requireOwnedCard(db, action.id);
    if (card.status == static_cast<int>(CardStatus::Cancelled)) {
        throw ConflictError{"cancelled cards cannot be reactivated"};
    }
    card.status = static_cast<int>(CardStatus::Active);
    db.cards.update(card);
    return dto::CommandResult{.ok = true, .message = "card active"};
}

dto::CommandResult CardModel::execute(const dto::CancelCard& action) {
    auto& db = wasm::sharedDb();
    auto card = requireOwnedCard(db, action.id);
    card.status = static_cast<int>(CardStatus::Cancelled);
    db.cards.update(card);
    return dto::CommandResult{.ok = true, .message = "card cancelled"};
}

dto::CommandResult CardModel::execute(const dto::SetCardLimit& action) {
    if (action.dailyLimitMinor < 0) {
        throw ValidationError{"limit must be non-negative"};
    }
    auto& db = wasm::sharedDb();
    auto card = requireOwnedCard(db, action.id);
    card.dailyLimitMinor = action.dailyLimitMinor;
    db.cards.update(card);
    return dto::CommandResult{.ok = true, .message = "limit updated"};
}

dto::CommandResult CardModel::execute(const dto::ChangePin& action) {
    if (!action.validate()) {
        throw ValidationError{"PIN must be exactly 4 digits"};
    }
    auto& db = wasm::sharedDb();
    auto card = requireOwnedCard(db, action.id);
    card.pinHash = hashPin(action.newPin);
    db.cards.update(card);
    return dto::CommandResult{.ok = true, .message = "PIN changed"};
}

dto::CardList CardModel::execute(const dto::ListCards& action) {
    const std::string owner = resolveOwner(action.owner);
    if (owner.empty()) {
        throw Unauthorized{"no session principal"};
    }
    auto& db = wasm::sharedDb();
    const auto ownerId = wasm::requireUserId(db, owner);
    dto::CardList out;
    for (const auto& rec : db.cards.where([&](const wasm::CardRow& c) { return c.userId == ownerId; })) {
        out.cards.push_back(toInfo(rec, owner));
    }
    return out;
}

}  // namespace bank

// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <morph/core/bridge.hpp>
#include <string>

#include "bank/app/app.hpp"
#include "bank/core/errors.hpp"
#include "bank/core/types.hpp"
#include "bank/dto/account_dto.hpp"
#include "bank/dto/card_dto.hpp"
#include "bank/models/card_model.hpp"
#include "bank/models/customer_model.hpp"
#include "bank_test_support.hpp"

using bank::testing::await;

namespace {

std::string testConnection() {
    bank::testing::ensureDatabase();
    return "DRIVER=SQLite3;Database=" + (std::filesystem::temp_directory_path() / "morph_bank_tests.db").string();
}

}  // namespace

TEST_CASE("CardModel issues and manages cards", "[card]") {
    bank::app::App app{testConnection()};
    app.login("judy-card");
    morph::bridge::BridgeHandler<bank::CustomerModel> accounts{app.bridge(), app.gui()};
    morph::bridge::BridgeHandler<bank::CardModel> cards{app.bridge(), app.gui()};

    const auto account = await(accounts.execute(bank::dto::OpenAccount{.kind = 0, .currency = 0}), app.guiLoop()).id;

    SECTION("issuing a card returns an active card with last-4") {
        auto card = await(
            cards.execute(bank::dto::IssueCard{
                .accountId = account, .kind = static_cast<int>(bank::CardKind::Debit), .dailyLimitMinor = 100000}),
            app.guiLoop());
        REQUIRE(card.id > 0);
        REQUIRE(card.status == static_cast<int>(bank::CardStatus::Active));
        REQUIRE(card.panLast4.size() == 4);
    }

    SECTION("freeze/unfreeze and limit/pin changes work") {
        auto card = await(cards.execute(bank::dto::IssueCard{.accountId = account, .kind = 0}), app.guiLoop());

        REQUIRE(await(cards.execute(bank::dto::FreezeCard{.id = card.id}), app.guiLoop()).ok);
        REQUIRE(await(cards.execute(bank::dto::UnfreezeCard{.id = card.id}), app.guiLoop()).ok);
        REQUIRE(
            await(cards.execute(bank::dto::SetCardLimit{.id = card.id, .dailyLimitMinor = 5000}), app.guiLoop()).ok);
        REQUIRE(await(cards.execute(bank::dto::ChangePin{.id = card.id, .newPin = "1357"}), app.guiLoop()).ok);
    }

    SECTION("a 3-digit PIN is rejected and cancelled cards cannot be unfrozen") {
        auto card = await(cards.execute(bank::dto::IssueCard{.accountId = account, .kind = 0}), app.guiLoop());
        // ChangePin::validate() already rejects a non-4-digit PIN, so morph's
        // ActionValidator gate catches this before CardModel::execute() runs
        // -- see docs/spec/forms/forms.md, "Security / trust boundary".
        REQUIRE_THROWS_AS(await(cards.execute(bank::dto::ChangePin{.id = card.id, .newPin = "12"}), app.guiLoop()),
                          morph::model::ValidationError);

        await(cards.execute(bank::dto::CancelCard{.id = card.id}), app.guiLoop());
        REQUIRE_THROWS_AS(await(cards.execute(bank::dto::UnfreezeCard{.id = card.id}), app.guiLoop()),
                          bank::ConflictError);
    }
}

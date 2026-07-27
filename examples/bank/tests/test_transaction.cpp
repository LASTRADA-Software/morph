// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <morph/core/bridge.hpp>
#include <string>

#include "bank/app/app.hpp"
#include "bank/core/errors.hpp"
#include "bank/core/types.hpp"
#include "bank/dto/account_dto.hpp"
#include "bank/dto/transaction_dto.hpp"
#include "bank/models/account_model.hpp"
#include "bank/models/transaction_model.hpp"
#include "bank_test_support.hpp"

using bank::testing::await;

namespace {

std::string testConnection() {
    bank::testing::ensureDatabase();
    return "DRIVER=SQLite3;Database=" + (std::filesystem::temp_directory_path() / "morph_bank_tests.db").string();
}

/// Opens a fresh checking account in the given currency and returns its id.
std::int64_t openAccount(bank::app::App& app, morph::bridge::BridgeHandler<bank::AccountModel>& accounts,
                         bank::Currency currency = bank::Currency::USD) {
    auto info = await(accounts.execute(bank::dto::OpenAccount{
                          .kind = static_cast<int>(bank::AccountKind::Checking),
                          .currency = static_cast<int>(currency),
                          .overdraftMinor = 0,
                      }),
                      app.guiLoop());
    return info.id;
}

}  // namespace

TEST_CASE("TransactionModel deposit / withdraw adjust balances and ledger", "[transaction]") {
    bank::app::App app{testConnection()};
    app.login("erin-txn");
    morph::bridge::BridgeHandler<bank::AccountModel> accounts{app.bridge(), app.gui()};
    morph::bridge::BridgeHandler<bank::TransactionModel> txns{app.bridge(), app.gui()};

    const std::int64_t acct = openAccount(app, accounts);

    SECTION("deposit increases the balance and records a credit") {
        auto entry =
            await(txns.execute(bank::dto::Deposit{.accountId = acct, .amountMinor = 10000, .description = "paycheck"}),
                  app.guiLoop());
        REQUIRE(entry.balanceAfterMinor == 10000);
        REQUIRE(entry.direction == static_cast<int>(bank::TxnDirection::Credit));
        REQUIRE(entry.kind == static_cast<int>(bank::TxnKind::Deposit));

        auto fetched = await(accounts.execute(bank::dto::GetAccount{.id = acct}), app.guiLoop());
        REQUIRE(fetched.balanceMinor == 10000);
    }

    SECTION("withdrawal beyond balance+overdraft is rejected") {
        await(txns.execute(bank::dto::Deposit{.accountId = acct, .amountMinor = 5000}), app.guiLoop());
        REQUIRE_THROWS_AS(
            await(txns.execute(bank::dto::Withdraw{.accountId = acct, .amountMinor = 6000}), app.guiLoop()),
            bank::InsufficientFunds);
    }

    SECTION("history returns entries newest-first") {
        await(txns.execute(bank::dto::Deposit{.accountId = acct, .amountMinor = 100}), app.guiLoop());
        await(txns.execute(bank::dto::Deposit{.accountId = acct, .amountMinor = 200}), app.guiLoop());
        await(txns.execute(bank::dto::Withdraw{.accountId = acct, .amountMinor = 50}), app.guiLoop());

        auto page = await(txns.execute(bank::dto::History{.accountId = acct, .limit = 10}), app.guiLoop());
        REQUIRE(page.entries.size() == 3);
        REQUIRE(page.entries.front().id > page.entries.back().id);  // newest first
    }
}

TEST_CASE("TransactionModel transfer is atomic and balance-preserving", "[transaction]") {
    bank::app::App app{testConnection()};
    app.login("frank-transfer");
    morph::bridge::BridgeHandler<bank::AccountModel> accounts{app.bridge(), app.gui()};
    morph::bridge::BridgeHandler<bank::TransactionModel> txns{app.bridge(), app.gui()};

    const std::int64_t src = openAccount(app, accounts);
    const std::int64_t dst = openAccount(app, accounts);
    await(txns.execute(bank::dto::Deposit{.accountId = src, .amountMinor = 10000}), app.guiLoop());

    SECTION("a valid transfer moves money and conserves the total") {
        auto result = await(txns.execute(bank::dto::Transfer{
                                .fromAccountId = src, .toAccountId = dst, .amountMinor = 4000, .description = "rent"}),
                            app.guiLoop());
        REQUIRE(result.fromBalanceMinor == 6000);
        REQUIRE(result.toBalanceMinor == 4000);
        REQUIRE(result.fromBalanceMinor + result.toBalanceMinor == 10000);
    }

    SECTION("transferring to the same account fails validation") {
        // Transfer::validate() (a field-level check) already rejects
        // fromAccountId == toAccountId, so morph's ActionValidator gate
        // catches this before TransactionModel::execute() runs -- see
        // docs/spec/forms/forms.md, "Security / trust boundary".
        REQUIRE_THROWS_AS(
            await(txns.execute(bank::dto::Transfer{.fromAccountId = src, .toAccountId = src, .amountMinor = 100}),
                  app.guiLoop()),
            morph::model::ValidationError);
    }

    SECTION("an over-balance transfer leaves both balances untouched") {
        REQUIRE_THROWS_AS(
            await(txns.execute(bank::dto::Transfer{.fromAccountId = src, .toAccountId = dst, .amountMinor = 999999}),
                  app.guiLoop()),
            bank::InsufficientFunds);
        auto srcInfo = await(accounts.execute(bank::dto::GetAccount{.id = src}), app.guiLoop());
        auto dstInfo = await(accounts.execute(bank::dto::GetAccount{.id = dst}), app.guiLoop());
        REQUIRE(srcInfo.balanceMinor == 10000);
        REQUIRE(dstInfo.balanceMinor == 0);
    }

    SECTION("a different principal cannot move money from accounts they do not own") {
        app.login("mallory-intruder");
        REQUIRE_THROWS_AS(
            await(txns.execute(bank::dto::Withdraw{.accountId = src, .amountMinor = 100}), app.guiLoop()),
            bank::Unauthorized);
        REQUIRE_THROWS_AS(
            await(txns.execute(bank::dto::Transfer{.fromAccountId = src, .toAccountId = dst, .amountMinor = 100}),
                  app.guiLoop()),
            bank::Unauthorized);
    }
}

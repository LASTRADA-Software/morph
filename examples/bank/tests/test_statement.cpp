// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <morph/core/bridge.hpp>
#include <string>

#include "bank/app/app.hpp"
#include "bank/dto/account_dto.hpp"
#include "bank/dto/statement_dto.hpp"
#include "bank/dto/transaction_dto.hpp"
#include "bank/models/customer_model.hpp"
#include "bank/models/statement_model.hpp"
#include "bank/models/transaction_model.hpp"
#include "bank_test_support.hpp"

using bank::testing::await;

namespace {

std::string testConnection() {
    bank::testing::ensureDatabase();
    return "DRIVER=SQLite3;Database=" + (std::filesystem::temp_directory_path() / "morph_bank_tests.db").string();
}

}  // namespace

TEST_CASE("StatementModel aggregates credits and debits across accounts", "[statement]") {
    bank::app::App app{testConnection()};
    app.login("nina-stmt");
    morph::bridge::BridgeHandler<bank::CustomerModel> accounts{app.bridge(), app.gui()};
    morph::bridge::BridgeHandler<bank::TransactionModel> txns{app.bridge(), app.gui()};
    morph::bridge::BridgeHandler<bank::StatementModel> statements{app.bridge(), app.gui()};

    const auto acctA = await(accounts.execute(bank::dto::OpenAccount{.kind = 0, .currency = 0}), app.guiLoop()).id;
    const auto acctB = await(accounts.execute(bank::dto::OpenAccount{.kind = 1, .currency = 0}), app.guiLoop()).id;

    await(txns.execute(bank::dto::Deposit{.accountId = acctA, .amountMinor = 10000}), app.guiLoop());
    await(txns.execute(bank::dto::Withdraw{.accountId = acctA, .amountMinor = 2500}), app.guiLoop());
    await(txns.execute(bank::dto::Deposit{.accountId = acctB, .amountMinor = 7000}), app.guiLoop());

    auto stmt = await(statements.execute(bank::dto::GenerateStatement{}), app.guiLoop());

    REQUIRE(stmt.owner == "nina-stmt");
    REQUIRE(stmt.lines.size() >= 2);
    REQUIRE(stmt.totalCreditsMinor == 17000);
    REQUIRE(stmt.totalDebitsMinor == 2500);
}

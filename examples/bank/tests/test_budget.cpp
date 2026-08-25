// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <morph/core/bridge.hpp>
#include <string>

#include "bank/app/app.hpp"
#include "bank/core/types.hpp"
#include "bank/dto/account_dto.hpp"
#include "bank/dto/budget_dto.hpp"
#include "bank/dto/transaction_dto.hpp"
#include "bank/models/budget_model.hpp"
#include "bank/models/customer_model.hpp"
#include "bank/models/transaction_model.hpp"
#include "bank_test_support.hpp"

using bank::testing::await;

namespace {

std::string testConnection() {
    bank::testing::ensureDatabase();
    return "DRIVER=SQLite3;Database=" + (std::filesystem::temp_directory_path() / "morph_bank_tests.db").string();
}

}  // namespace

TEST_CASE("BudgetModel upserts budgets and computes spending", "[budget]") {
    bank::app::App app{testConnection()};
    app.login("laura-budget");
    morph::bridge::BridgeHandler<bank::CustomerModel> accounts{app.bridge(), app.gui()};
    morph::bridge::BridgeHandler<bank::TransactionModel> txns{app.bridge(), app.gui()};
    morph::bridge::BridgeHandler<bank::BudgetModel> budgets{app.bridge(), app.gui()};

    SECTION("setting the same category twice updates rather than duplicates") {
        await(budgets.execute(bank::dto::SetBudget{.category = "groceries", .monthlyLimitMinor = 30000}),
              app.guiLoop());
        await(budgets.execute(bank::dto::SetBudget{.category = "groceries", .monthlyLimitMinor = 45000}),
              app.guiLoop());

        auto list = await(budgets.execute(bank::dto::ListBudgets{}), app.guiLoop());
        int groceries = 0;
        for (const auto& budget : list.budgets) {
            if (budget.category == "groceries") {
                ++groceries;
                REQUIRE(budget.monthlyLimitMinor == 45000);
            }
        }
        REQUIRE(groceries == 1);
    }

    SECTION("spending-by-kind sums debits from the ledger") {
        const auto acct = await(accounts.execute(bank::dto::OpenAccount{.kind = 0, .currency = 0}), app.guiLoop()).id;
        await(txns.execute(bank::dto::Deposit{.accountId = acct, .amountMinor = 100000}), app.guiLoop());
        await(txns.execute(bank::dto::Withdraw{.accountId = acct, .amountMinor = 3000}), app.guiLoop());
        await(txns.execute(bank::dto::Withdraw{.accountId = acct, .amountMinor = 2000}), app.guiLoop());

        auto report = await(budgets.execute(bank::dto::SpendingByKind{.accountId = acct}), app.guiLoop());
        REQUIRE(report.totalDebitsMinor == 5000);
        bool sawWithdrawals = false;
        for (const auto& spend : report.byKind) {
            if (spend.kind == static_cast<int>(bank::TxnKind::Withdrawal)) {
                sawWithdrawals = true;
                REQUIRE(spend.totalMinor == 5000);
                REQUIRE(spend.count == 2);
            }
        }
        REQUIRE(sawWithdrawals);
    }
}

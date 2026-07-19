// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>

#include <morph/core/bridge.hpp>

#include <filesystem>
#include <string>

#include "bank/app/app.hpp"
#include "bank/core/errors.hpp"
#include "bank/core/types.hpp"
#include "bank/dto/account_dto.hpp"
#include "bank/dto/loan_dto.hpp"
#include "bank/models/account_model.hpp"
#include "bank/models/loan_model.hpp"
#include "bank_test_support.hpp"

using bank::testing::await;

namespace {

std::string testConnection() {
    bank::testing::ensureDatabase();
    return "DRIVER=SQLite3;Database=" +
           (std::filesystem::temp_directory_path() / "morph_bank_tests.db").string();
}

}  // namespace

TEST_CASE("LoanModel disburses, schedules, and repays", "[loan]") {
    bank::app::App app{testConnection()};
    app.login("ken-loan");
    morph::bridge::BridgeHandler<bank::AccountModel> accounts{app.bridge(), app.gui()};
    morph::bridge::BridgeHandler<bank::LoanModel> loans{app.bridge(), app.gui()};

    const auto account =
        await(accounts.execute(bank::dto::OpenAccount{.kind = 0, .currency = 0}), app.guiLoop()).id;

    SECTION("applying disburses the principal into the account") {
        auto loan = await(loans.execute(bank::dto::ApplyLoan{.accountId = account,
                                                            .principalMinor = 1200000,
                                                            .rateBps = 600,
                                                            .termMonths = 12}),
                          app.guiLoop());
        REQUIRE(loan.outstandingMinor == 1200000);
        REQUIRE(loan.status == static_cast<int>(bank::LoanStatus::Active));

        auto acct = await(accounts.execute(bank::dto::GetAccount{.id = account}), app.guiLoop());
        REQUIRE(acct.balanceMinor == 1200000);
    }

    SECTION("the amortization schedule covers the term and clears the balance") {
        auto loan = await(loans.execute(bank::dto::ApplyLoan{.accountId = account,
                                                            .principalMinor = 1200000,
                                                            .rateBps = 600,
                                                            .termMonths = 12}),
                          app.guiLoop());
        auto schedule =
            await(loans.execute(bank::dto::LoanScheduleRequest{.loanId = loan.id}), app.guiLoop());
        REQUIRE(schedule.installments.size() == 12);
        REQUIRE(schedule.monthlyPaymentMinor > 0);
        REQUIRE(schedule.installments.back().remainingMinor == 0);
    }

    SECTION("repaying the full balance marks the loan paid off") {
        auto loan = await(loans.execute(bank::dto::ApplyLoan{.accountId = account,
                                                            .principalMinor = 100000,
                                                            .rateBps = 0,
                                                            .termMonths = 6}),
                          app.guiLoop());
        auto after = await(loans.execute(bank::dto::RepayLoan{.loanId = loan.id,
                                                             .fromAccountId = account,
                                                             .amountMinor = 100000}),
                           app.guiLoop());
        REQUIRE(after.outstandingMinor == 0);
        REQUIRE(after.status == static_cast<int>(bank::LoanStatus::PaidOff));
    }
}

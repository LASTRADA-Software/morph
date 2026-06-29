// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>

#include <morph/bridge.hpp>

#include <filesystem>
#include <string>

#include "bank/app/app.hpp"
#include "bank/core/errors.hpp"
#include "bank/core/types.hpp"
#include "bank/dto/account_dto.hpp"
#include "bank/dto/payee_dto.hpp"
#include "bank/dto/payment_dto.hpp"
#include "bank/models/account_model.hpp"
#include "bank/models/payee_model.hpp"
#include "bank/models/payment_model.hpp"
#include "bank/models/transaction_model.hpp"
#include "bank_test_support.hpp"

using bank::testing::await;

namespace {

std::string testConnection() {
    bank::testing::ensureDatabase();
    return "DRIVER=SQLite3;Database=" +
           (std::filesystem::temp_directory_path() / "morph_bank_tests.db").string();
}

}  // namespace

TEST_CASE("PaymentModel pays bills, schedules, and cancels", "[payment]") {
    bank::app::App app{testConnection()};
    app.login("ivan-pay");
    morph::bridge::BridgeHandler<bank::AccountModel> accounts{app.bridge(), app.gui()};
    morph::bridge::BridgeHandler<bank::PayeeModel> payees{app.bridge(), app.gui()};
    morph::bridge::BridgeHandler<bank::TransactionModel> txns{app.bridge(), app.gui()};
    morph::bridge::BridgeHandler<bank::PaymentModel> payments{app.bridge(), app.gui()};

    const auto account =
        await(accounts.execute(bank::dto::OpenAccount{.kind = 0, .currency = 0}), app.guiLoop()).id;
    await(txns.execute(bank::dto::Deposit{.accountId = account, .amountMinor = 50000}), app.guiLoop());
    const auto payee = await(payees.execute(bank::dto::AddPayee{.name = "Electric Co",
                                                               .iban = "DE89370400440532013000"}),
                             app.guiLoop())
                           .id;

    SECTION("paying a bill debits the account and records a completed payment") {
        auto info = await(payments.execute(bank::dto::PayBill{.fromAccountId = account,
                                                             .payeeId = payee,
                                                             .amountMinor = 12000,
                                                             .description = "March bill"}),
                          app.guiLoop());
        REQUIRE(info.status == static_cast<int>(bank::PaymentStatus::Completed));

        auto acct = await(accounts.execute(bank::dto::GetAccount{.id = account}), app.guiLoop());
        REQUIRE(acct.balanceMinor == 38000);
    }

    SECTION("scheduling and a standing order start pending and can be cancelled") {
        auto scheduled = await(payments.execute(bank::dto::SchedulePayment{.fromAccountId = account,
                                                                          .payeeId = payee,
                                                                          .amountMinor = 1000,
                                                                          .dueAtMs = 9999999999999}),
                               app.guiLoop());
        REQUIRE(scheduled.status == static_cast<int>(bank::PaymentStatus::Pending));
        REQUIRE(scheduled.schedule == static_cast<int>(bank::PaymentSchedule::Scheduled));

        auto standing = await(payments.execute(bank::dto::CreateStandingOrder{.fromAccountId = account,
                                                                             .payeeId = payee,
                                                                             .amountMinor = 2000,
                                                                             .intervalDays = 30,
                                                                             .firstDueAtMs = 9999999999999}),
                              app.guiLoop());
        REQUIRE(standing.schedule == static_cast<int>(bank::PaymentSchedule::Standing));
        REQUIRE(standing.intervalDays == 30);

        auto cancelled =
            await(payments.execute(bank::dto::CancelPayment{.id = scheduled.id}), app.guiLoop());
        REQUIRE(cancelled.ok);
    }

    SECTION("paying more than the balance is rejected") {
        REQUIRE_THROWS_AS(await(payments.execute(bank::dto::PayBill{.fromAccountId = account,
                                                                    .payeeId = payee,
                                                                    .amountMinor = 999999}),
                                app.guiLoop()),
                          bank::InsufficientFunds);
    }
}

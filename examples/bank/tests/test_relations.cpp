// SPDX-License-Identifier: Apache-2.0
//
// Exercises the Lightweight ORM relations the persistence layer is built on:
//   * BelongsTo  — navigating a foreign key to the referenced record
//                  (account.user, card.account, payment.payee/fromAccount, …)
//   * HasMany    — the inverse one-to-many (user.accounts, payee.payments)
//   * nullable BelongsTo — transactions.counterparty (set for transfers,
//                  NULL for deposits/withdrawals)
//
// Data is created through the real models/bridge (the same paths the GUI/CLI
// use); the relations are then read back directly through a DataMapper so the
// assertions are about what actually persisted.

#include <catch2/catch_test_macros.hpp>

#include <Lightweight/Lightweight.hpp>
#include <morph/core/bridge.hpp>

#include <cstdint>
#include <string>

#include "bank/app/app.hpp"
#include "bank/core/types.hpp"
#include "bank/db/entities.hpp"
#include "bank/db/user_ops.hpp"
#include "bank/dto/account_dto.hpp"
#include "bank/dto/card_dto.hpp"
#include "bank/dto/payee_dto.hpp"
#include "bank/dto/payment_dto.hpp"
#include "bank/dto/transaction_dto.hpp"
#include "bank/models/customer_model.hpp"
#include "bank/models/card_model.hpp"
#include "bank/models/payee_model.hpp"
#include "bank/models/payment_model.hpp"
#include "bank/models/transaction_model.hpp"
#include "bank_test_support.hpp"

using bank::testing::await;

namespace {

std::string dbConnectionForTests() {
    bank::testing::ensureDatabase();
    return "DRIVER=SQLite3;Database=" +
           (std::filesystem::temp_directory_path() / "morph_bank_tests.db").string();
}

}  // namespace

TEST_CASE("ORM relations: BelongsTo navigation and HasMany inverses", "[relations]") {
    const std::string principal = "rel-user";
    bank::app::App app{dbConnectionForTests()};
    app.login(principal);  // provisions the users row

    morph::bridge::BridgeHandler<bank::CustomerModel> accounts{app.bridge(), app.gui()};
    morph::bridge::BridgeHandler<bank::CardModel> cards{app.bridge(), app.gui()};
    morph::bridge::BridgeHandler<bank::PayeeModel> payees{app.bridge(), app.gui()};
    morph::bridge::BridgeHandler<bank::PaymentModel> payments{app.bridge(), app.gui()};
    morph::bridge::BridgeHandler<bank::TransactionModel> txns{app.bridge(), app.gui()};

    // Build a small graph: two accounts, a card, a payee, a one-off payment, a
    // deposit, and a transfer between the accounts.
    auto checking = await(accounts.execute(bank::dto::OpenAccount{
                              .kind = static_cast<int>(bank::AccountKind::Checking),
                              .currency = static_cast<int>(bank::Currency::EUR)}),
                          app.guiLoop());
    auto savings = await(accounts.execute(bank::dto::OpenAccount{
                             .kind = static_cast<int>(bank::AccountKind::Savings),
                             .currency = static_cast<int>(bank::Currency::EUR)}),
                         app.guiLoop());
    auto card = await(cards.execute(bank::dto::IssueCard{.accountId = checking.id, .kind = 0}),
                      app.guiLoop());
    auto payee = await(payees.execute(bank::dto::AddPayee{
                           .name = "Landlord", .iban = "DE89370400440532013000", .bankName = "ACME"}),
                       app.guiLoop());
    await(txns.execute(bank::dto::Deposit{.accountId = checking.id, .amountMinor = 100000}), app.guiLoop());
    auto payment = await(payments.execute(bank::dto::PayBill{.fromAccountId = checking.id,
                                                            .payeeId = payee.id,
                                                            .amountMinor = 5000,
                                                            .description = "rent"}),
                         app.guiLoop());
    await(txns.execute(bank::dto::Transfer{.fromAccountId = checking.id,
                                           .toAccountId = savings.id,
                                           .amountMinor = 30000}),
          app.guiLoop());

    // Read the persisted graph back through the relation API.
    Lightweight::DataMapper dm;
    const auto userId = bank::db::requireUserId(dm, principal);

    SECTION("BelongsTo: account.user navigates to the owning user") {
        auto acct = dm.QuerySingle<bank::db::AccountRecord>(static_cast<std::uint64_t>(checking.id)).value();
        REQUIRE(acct.user.Value() == userId);
        REQUIRE(std::string{acct.user->username.Value().str()} == principal);  // lazy navigation
    }

    SECTION("BelongsTo: card.account and card.user resolve") {
        auto rec = dm.QuerySingle<bank::db::CardRecord>(static_cast<std::uint64_t>(card.id)).value();
        REQUIRE(static_cast<std::int64_t>(rec.account.Value()) == checking.id);
        REQUIRE(std::string{rec.account->number.Value().str()} == checking.number);
        REQUIRE(std::string{rec.user->username.Value().str()} == principal);
    }

    SECTION("BelongsTo: payment.fromAccount and payment.payee resolve") {
        auto rec = dm.QuerySingle<bank::db::PaymentRecord>(static_cast<std::uint64_t>(payment.id)).value();
        REQUIRE(static_cast<std::int64_t>(rec.fromAccount.Value()) == checking.id);
        REQUIRE(static_cast<std::int64_t>(rec.payee.Value()) == payee.id);
        REQUIRE(std::string{rec.payee->name.Value().str()} == "Landlord");
        REQUIRE(std::string{rec.fromAccount->number.Value().str()} == checking.number);
    }

    SECTION("HasMany: user.accounts contains the opened accounts") {
        auto user = dm.QuerySingle<bank::db::UserRecord>(userId).value();
        REQUIRE(user.accounts.Count() >= 2);
        bool sawChecking = false;
        bool sawSavings = false;
        for (const auto& acct : user.accounts.All()) {
            const auto id = static_cast<std::int64_t>(acct->id.Value());
            sawChecking = sawChecking || id == checking.id;
            sawSavings = sawSavings || id == savings.id;
            REQUIRE(acct->user.Value() == userId);  // every child points back
        }
        REQUIRE(sawChecking);
        REQUIRE(sawSavings);
    }

    SECTION("HasMany: payee.payments contains the bill payment") {
        auto rec = dm.QuerySingle<bank::db::PayeeRecord>(static_cast<std::uint64_t>(payee.id)).value();
        REQUIRE(rec.payments.Count() == 1);
        REQUIRE(static_cast<std::int64_t>(rec.payments[0].id.Value()) == payment.id);
        // navigate the "many" side back to its "one" side
        REQUIRE(static_cast<std::int64_t>(rec.payments[0].payee.Value()) == payee.id);
    }

    SECTION("nullable BelongsTo: counterparty is set for transfers, NULL otherwise") {
        auto rows = dm.Query<bank::db::TxnRecord>()
                        .Where(Lightweight::FieldNameOf<&bank::db::TxnRecord::account>, "=", checking.id)
                        .All();
        bool sawDepositWithoutCounterparty = false;
        bool sawTransferWithCounterparty = false;
        for (const auto& txn : rows) {
            if (txn.kind.Value() == static_cast<int>(bank::TxnKind::Deposit)) {
                REQUIRE_FALSE(static_cast<bool>(txn.counterparty));  // NULL
                sawDepositWithoutCounterparty = true;
            }
            if (txn.kind.Value() == static_cast<int>(bank::TxnKind::TransferOut)) {
                REQUIRE(static_cast<bool>(txn.counterparty));
                REQUIRE(static_cast<std::int64_t>(txn.counterparty.Value().value()) == savings.id);
                sawTransferWithCounterparty = true;
            }
        }
        REQUIRE(sawDepositWithoutCounterparty);
        REQUIRE(sawTransferWithCounterparty);
    }
}

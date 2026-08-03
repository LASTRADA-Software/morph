// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <morph/core/bridge.hpp>
#include <string>

#include "bank/app/app.hpp"
#include "bank/core/errors.hpp"
#include "bank/core/types.hpp"
#include "bank/dto/account_dto.hpp"
#include "bank/models/account_model.hpp"
#include "bank/models/customer_model.hpp"
#include "bank_test_support.hpp"

using bank::testing::await;

namespace {

/// Builds an App against the shared test DB and logs in @p principal.
std::string dbConnectionForTests() {
    bank::testing::ensureDatabase();
    return "DRIVER=SQLite3;Database=" + (std::filesystem::temp_directory_path() / "morph_bank_tests.db").string();
}

}  // namespace

TEST_CASE("AccountModel opens, lists, fetches and closes accounts", "[account]") {
    bank::app::App app{dbConnectionForTests()};
    app.login("alice-account-basic");

    morph::bridge::BridgeHandler<bank::AccountModel> accounts{app.bridge(), app.gui()};
    morph::bridge::BridgeHandler<bank::CustomerModel> accountsOwner{app.bridge(), app.gui()};

    SECTION("open populates an account with a number and zero balance") {
        auto info = await(accountsOwner.execute(bank::dto::OpenAccount{
                              .owner = "",
                              .kind = static_cast<int>(bank::AccountKind::Checking),
                              .currency = static_cast<int>(bank::Currency::EUR),
                              .overdraftMinor = 50000,
                          }),
                          app.guiLoop());

        REQUIRE(info.id > 0);
        REQUIRE(info.owner == "alice-account-basic");
        REQUIRE(info.number.size() == 22);  // "DE" + 20 digits
        REQUIRE(info.balanceMinor == 0);
        REQUIRE(info.overdraftMinor == 50000);
        REQUIRE(info.status == static_cast<int>(bank::AccountStatus::Open));
        REQUIRE(info.currency == static_cast<int>(bank::Currency::EUR));
    }

    SECTION("savings accounts get a non-zero interest rate") {
        auto info = await(accountsOwner.execute(bank::dto::OpenAccount{
                              .kind = static_cast<int>(bank::AccountKind::Savings),
                              .currency = static_cast<int>(bank::Currency::USD),
                          }),
                          app.guiLoop());
        REQUIRE(info.interestBps > 0);
    }

    SECTION("list returns only the session owner's accounts") {
        await(accountsOwner.execute(bank::dto::OpenAccount{.kind = 0, .currency = 0}), app.guiLoop());
        await(accountsOwner.execute(bank::dto::OpenAccount{.kind = 1, .currency = 0}), app.guiLoop());

        auto list = await(accountsOwner.execute(bank::dto::ListAccounts{}), app.guiLoop());
        REQUIRE(list.accounts.size() >= 2);
        for (const auto& acct : list.accounts) {
            REQUIRE(acct.owner == "alice-account-basic");
        }
    }

    SECTION("get returns the same account that was opened") {
        auto opened = await(accountsOwner.execute(bank::dto::OpenAccount{.kind = 0, .currency = 0}), app.guiLoop());
        auto fetched = await(accounts.execute(bank::dto::GetAccount{.id = opened.id}), app.guiLoop());
        REQUIRE(fetched.id == opened.id);
        REQUIRE(fetched.number == opened.number);
    }

    SECTION("closing a zero-balance account succeeds") {
        auto opened = await(accountsOwner.execute(bank::dto::OpenAccount{.kind = 0, .currency = 0}), app.guiLoop());
        auto result = await(accounts.execute(bank::dto::CloseAccount{.id = opened.id}), app.guiLoop());
        REQUIRE(result.ok);

        auto fetched = await(accounts.execute(bank::dto::GetAccount{.id = opened.id}), app.guiLoop());
        REQUIRE(fetched.status == static_cast<int>(bank::AccountStatus::Closed));
    }
}

TEST_CASE("AccountModel reports errors through onError", "[account]") {
    bank::app::App app{dbConnectionForTests()};
    app.login("bob-account-errors");
    morph::bridge::BridgeHandler<bank::AccountModel> accounts{app.bridge(), app.gui()};
    morph::bridge::BridgeHandler<bank::CustomerModel> accountsOwner{app.bridge(), app.gui()};

    SECTION("fetching a non-existent account throws NotFound") {
        REQUIRE_THROWS_AS(await(accounts.execute(bank::dto::GetAccount{.id = 999999}), app.guiLoop()), bank::NotFound);
    }

    SECTION("invalid currency fails validation") {
        // OpenAccount::validate() already rejects an out-of-range currency,
        // so morph's ActionValidator gate catches this before AccountModel::
        // execute() runs -- see docs/spec/forms/forms.md, "Security / trust
        // boundary".
        REQUIRE_THROWS_AS(await(accountsOwner.execute(bank::dto::OpenAccount{.kind = 0, .currency = 99}), app.guiLoop()),
                          morph::model::ValidationError);
    }
}

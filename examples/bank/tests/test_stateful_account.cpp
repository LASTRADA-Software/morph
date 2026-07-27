// SPDX-License-Identifier: Apache-2.0
//
// Tests for the stateful, keyed AccountModel (F1).
//
// The point of the reshape is that an account instance *holds* its row rather
// than re-querying it, and that two handlers naming the same account reach one
// instance. A stateless model would satisfy none of these assertions
// non-vacuously — the balance would simply be re-read from SQLite every time.
//
// See docs/planned/stateful_bank_example.md.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <filesystem>
#include <morph/core/bridge.hpp>
#include <string>

#include "bank/app/app.hpp"
#include "bank/core/types.hpp"
#include "bank/dto/account_dto.hpp"
#include "bank/dto/transaction_dto.hpp"
#include "bank/models/account_model.hpp"
#include "bank/models/customer_model.hpp"
#include "bank/models/transaction_model.hpp"
#include "bank_test_support.hpp"

using bank::testing::await;
using morph::bridge::AllowShared;
using morph::bridge::BridgeHandler;

namespace {

std::string statefulTestConnection() {
    bank::testing::ensureDatabase();
    return "DRIVER=SQLite3;Database=" + (std::filesystem::temp_directory_path() / "morph_bank_tests.db").string();
}

/// Opens a checking account for the logged-in principal and returns its id.
std::int64_t openChecking(bank::app::App& app, BridgeHandler<bank::CustomerModel>& customer) {
    auto info = await(customer.execute(bank::dto::OpenAccount{
                          .kind = static_cast<int>(bank::AccountKind::Checking),
                          .currency = static_cast<int>(bank::Currency::USD),
                          .overdraftMinor = 0,
                      }),
                      app.guiLoop());
    return info.id;
}

}  // namespace

TEST_CASE("two shared handlers on one account reach one instance", "[stateful-account]") {
    bank::app::App app{statefulTestConnection()};
    app.login("sid-shared-instance");

    BridgeHandler<bank::CustomerModel> customer{app.bridge(), app.gui()};
    const auto acct = openChecking(app, customer);

    BridgeHandler<bank::AccountModel, AllowShared> screen{app.bridge(), app.gui()};
    BridgeHandler<bank::AccountModel, AllowShared> sidebar{app.bridge(), app.gui()};

    // Each keyed action attaches its handler to the account it names, so both
    // land on the same instance rather than on two copies of the same row.
    REQUIRE(await(screen.execute(bank::dto::GetAccount{.id = acct}), app.guiLoop()).id == acct);
    REQUIRE(await(sidebar.execute(bank::dto::GetAccount{.id = acct}), app.guiLoop()).id == acct);

    REQUIRE(screen.primary().value_or(-1) == acct);
    REQUIRE(sidebar.primary().value_or(-1) == acct);

    // The directory holds exactly one entry for the account both handlers named.
    REQUIRE(await(screen.instances(), app.guiLoop()) == std::vector<std::int64_t>{acct});
}

TEST_CASE("a cached account re-hydrates after another model moves money", "[stateful-account]") {
    bank::app::App app{statefulTestConnection()};
    app.login("tess-stale-cache");

    BridgeHandler<bank::CustomerModel> customer{app.bridge(), app.gui()};
    const auto acct = openChecking(app, customer);

    BridgeHandler<bank::AccountModel, AllowShared> account{app.bridge(), app.gui()};
    BridgeHandler<bank::TransactionModel> txns{app.bridge(), app.gui()};

    REQUIRE(await(account.execute(bank::dto::GetAccount{.id = acct}), app.guiLoop()).balanceMinor == 0);

    // TransactionModel owns the atomic write and settles on its *own* SQLite
    // connection, so the cached row above is now stale. The version bump is what
    // makes the next read notice — without it a stateful model would happily
    // serve money that no longer exists.
    await(txns.execute(bank::dto::Deposit{.accountId = acct, .amountMinor = 12345}), app.guiLoop());

    REQUIRE(await(account.execute(bank::dto::GetAccount{.id = acct}), app.guiLoop()).balanceMinor == 12345);
}

TEST_CASE("a plain account handler keeps its own instance", "[stateful-account]") {
    bank::app::App app{statefulTestConnection()};
    app.login("percy-private-instance");

    BridgeHandler<bank::CustomerModel> customer{app.bridge(), app.gui()};
    const auto acct = openChecking(app, customer);

    BridgeHandler<bank::AccountModel, AllowShared> shared{app.bridge(), app.gui()};
    BridgeHandler<bank::AccountModel> priv{app.bridge(), app.gui()};

    REQUIRE(await(shared.execute(bank::dto::GetAccount{.id = acct}), app.guiLoop()).id == acct);
    REQUIRE(await(priv.execute(bank::dto::GetAccount{.id = acct}), app.guiLoop()).id == acct);

    // Both answer correctly — they read the same row — but only the opted-in
    // handler is in the directory, so the plain one is invisible to it.
    REQUIRE(await(shared.instances(), app.guiLoop()).size() == 1);
}

TEST_CASE("closing through the cached instance still enforces the zero-balance rule", "[stateful-account]") {
    bank::app::App app{statefulTestConnection()};
    app.login("cass-close-guard");

    BridgeHandler<bank::CustomerModel> customer{app.bridge(), app.gui()};
    const auto acct = openChecking(app, customer);

    BridgeHandler<bank::AccountModel, AllowShared> account{app.bridge(), app.gui()};
    BridgeHandler<bank::TransactionModel> txns{app.bridge(), app.gui()};

    await(txns.execute(bank::dto::Deposit{.accountId = acct, .amountMinor = 500}), app.guiLoop());

    // The guard reads the cached row, so it only holds because the deposit
    // invalidated that cache.
    auto refused = await(account.execute(bank::dto::CloseAccount{.id = acct}), app.guiLoop());
    REQUIRE_FALSE(refused.ok);

    await(txns.execute(bank::dto::Withdraw{.accountId = acct, .amountMinor = 500}), app.guiLoop());
    auto accepted = await(account.execute(bank::dto::CloseAccount{.id = acct}), app.guiLoop());
    REQUIRE(accepted.ok);

    REQUIRE(await(account.execute(bank::dto::GetAccount{.id = acct}), app.guiLoop()).status ==
            static_cast<int>(bank::AccountStatus::Closed));
}

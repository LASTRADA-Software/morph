// SPDX-License-Identifier: Apache-2.0
//
// Demonstrates the morph offline stack with real bank actions: while
// "offline", actions are serialised (via the same ActionTraits codec the wire
// uses) and parked in an IOfflineQueue. On "reconnect", a SyncWorker drains the
// queue and replays each action through the live bridge handler.

#include <catch2/catch_test_macros.hpp>

#include <morph/core/bridge.hpp>
#include <morph/offline/offline_queue.hpp>
#include <morph/core/registry.hpp>
#include <morph/offline/sync_worker.hpp>

#include <filesystem>
#include <string>

#include "bank/app/app.hpp"
#include "bank/dto/account_dto.hpp"
#include "bank/dto/transaction_dto.hpp"
#include "bank/models/account_model.hpp"
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

TEST_CASE("Offline deposits are queued and replayed on reconnect", "[offline]") {
    bank::app::App app{testConnection()};
    app.login("peter-offline");
    morph::bridge::BridgeHandler<bank::AccountModel> accounts{app.bridge(), app.gui()};
    morph::bridge::BridgeHandler<bank::TransactionModel> txns{app.bridge(), app.gui()};

    const auto acct =
        await(accounts.execute(bank::dto::OpenAccount{.kind = 0, .currency = 0}), app.guiLoop()).id;

    // --- While "offline": park deposits in the durable queue instead of sending.
    morph::offline::InMemoryOfflineQueue queue;
    using Codec = morph::model::ActionTraits<bank::dto::Deposit>;
    queue.enqueue(Codec::toJson(bank::dto::Deposit{.accountId = acct, .amountMinor = 1500}));
    queue.enqueue(Codec::toJson(bank::dto::Deposit{.accountId = acct, .amountMinor = 2500}));

    // Nothing has been applied yet.
    REQUIRE(await(accounts.execute(bank::dto::GetAccount{.id = acct}), app.guiLoop()).balanceMinor == 0);

    // --- On "reconnect": drain the queue, replaying each action via the bridge.
    morph::offline::SyncWorker worker{queue, [&](const std::string& payload) -> bool {
                                          try {
                                              await(txns.execute(Codec::fromJson(payload)), app.guiLoop());
                                              return true;
                                          } catch (...) {
                                              return false;
                                          }
                                      }};
    auto result = worker.run();

    REQUIRE(result.successful == 2);
    REQUIRE(result.failed == 0);
    REQUIRE(await(accounts.execute(bank::dto::GetAccount{.id = acct}), app.guiLoop()).balanceMinor == 4000);
}

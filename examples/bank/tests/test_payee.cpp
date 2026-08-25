// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <morph/core/bridge.hpp>
#include <string>

#include "bank/app/app.hpp"
#include "bank/core/errors.hpp"
#include "bank/dto/payee_dto.hpp"
#include "bank/models/payee_model.hpp"
#include "bank_test_support.hpp"

using bank::testing::await;
using bank::testing::waitUntil;

namespace {

std::string testConnection() {
    bank::testing::ensureDatabase();
    return "DRIVER=SQLite3;Database=" + (std::filesystem::temp_directory_path() / "morph_bank_tests.db").string();
}

}  // namespace

TEST_CASE("PayeeModel add/list/remove scoped to the owner", "[payee]") {
    bank::app::App app{testConnection()};
    app.login("grace-payee");
    morph::bridge::BridgeHandler<bank::PayeeModel> payees{app.bridge(), app.gui()};

    SECTION("a valid payee is saved and listed") {
        auto added = await(payees.execute(bank::dto::AddPayee{
                               .name = "Landlord", .iban = "DE89370400440532013000", .bankName = "Deutsche Bank"}),
                           app.guiLoop());
        REQUIRE(added.id > 0);
        REQUIRE(added.owner == "grace-payee");

        auto list = await(payees.execute(bank::dto::ListPayees{}), app.guiLoop());
        REQUIRE_FALSE(list.payees.empty());
    }

    SECTION("an implausible IBAN is rejected") {
        // AddPayee::validate() already rejects a non-IBAN-shaped string, so
        // morph's ActionValidator gate catches this before PayeeModel::
        // execute() runs -- see docs/spec/forms/forms.md, "Security / trust
        // boundary".
        REQUIRE_THROWS_AS(await(payees.execute(bank::dto::AddPayee{.name = "Bad", .iban = "123"}), app.guiLoop()),
                          morph::model::ValidationError);
    }

    SECTION("removing a payee succeeds") {
        auto added = await(payees.execute(bank::dto::AddPayee{.name = "Utility", .iban = "GB29NWBK60161331926819"}),
                           app.guiLoop());
        auto removed = await(payees.execute(bank::dto::RemovePayee{.id = added.id}), app.guiLoop());
        REQUIRE(removed.ok);
    }
}

TEST_CASE("PayeeModel notifies subscribers of the state it produces", "[payee][subscribe]") {
    bank::app::App app{testConnection()};
    app.login("heidi-form");
    morph::bridge::BridgeHandler<bank::PayeeModel> payees{app.bridge(), app.gui()};

    // Subscribing names the result type, so the observer never has to know
    // which action produced it.
    std::atomic<bool> fired{false};
    std::int64_t newId = 0;
    payees.subscribe<bank::dto::PayeeInfo>([&](const bank::dto::PayeeInfo& info) {
        newId = info.id;
        fired.store(true);
    });

    // An incomplete payee fails validate() (no IBAN), so the dispatch-path
    // validator rejects it and no subscriber is notified.
    std::atomic<bool> rejected{false};
    payees.execute(bank::dto::AddPayee{.name = "Streamed Payee", .iban = ""}).onError([&](const std::exception_ptr&) {
        rejected.store(true);
    });
    REQUIRE(waitUntil([&] { return rejected.load(); }, std::chrono::milliseconds{2000}, std::chrono::milliseconds{5},
                      app.guiLoop()));
    REQUIRE_FALSE(fired.load());

    // A complete one succeeds and the subscription fires.
    await(payees.execute(bank::dto::AddPayee{.name = "Streamed Payee", .iban = "FR1420041010050500013M02606"}),
          app.guiLoop());
    REQUIRE(waitUntil([&] { return fired.load(); }, std::chrono::milliseconds{2000}, std::chrono::milliseconds{5},
                      app.guiLoop()));
    REQUIRE(newId > 0);
}

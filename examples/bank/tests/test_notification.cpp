// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>

#include <morph/bridge.hpp>

#include <filesystem>
#include <string>

#include "bank/app/app.hpp"
#include "bank/dto/notification_dto.hpp"
#include "bank/models/notification_model.hpp"
#include "bank_test_support.hpp"

using bank::testing::await;

namespace {

std::string testConnection() {
    bank::testing::ensureDatabase();
    return "DRIVER=SQLite3;Database=" +
           (std::filesystem::temp_directory_path() / "morph_bank_tests.db").string();
}

}  // namespace

TEST_CASE("NotificationModel posts, lists, and marks read", "[notification]") {
    bank::app::App app{testConnection()};
    app.login("mike-notify");
    morph::bridge::BridgeHandler<bank::NotificationModel> notes{app.bridge(), app.gui()};

    auto first = await(notes.execute(bank::dto::Notify{.message = "Low balance", .severity = 1}),
                       app.guiLoop());
    await(notes.execute(bank::dto::Notify{.message = "Large transaction", .severity = 2}), app.guiLoop());

    SECTION("unread count and unread-only filter reflect state") {
        auto all = await(notes.execute(bank::dto::ListNotifications{}), app.guiLoop());
        REQUIRE(all.unreadCount == 2);

        await(notes.execute(bank::dto::MarkRead{.id = first.id}), app.guiLoop());
        auto unread =
            await(notes.execute(bank::dto::ListNotifications{.unreadOnly = true}), app.guiLoop());
        REQUIRE(unread.notifications.size() == 1);
    }

    SECTION("mark-all-read clears the unread count") {
        await(notes.execute(bank::dto::MarkAllRead{}), app.guiLoop());
        auto after = await(notes.execute(bank::dto::ListNotifications{}), app.guiLoop());
        REQUIRE(after.unreadCount == 0);
    }
}

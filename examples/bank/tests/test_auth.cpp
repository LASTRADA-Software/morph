// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>

#include <morph/bridge.hpp>

#include <filesystem>
#include <string>

#include "bank/app/app.hpp"
#include "bank/core/errors.hpp"
#include "bank/dto/auth_dto.hpp"
#include "bank/models/auth_model.hpp"
#include "bank_test_support.hpp"

using bank::testing::await;

namespace {

std::string testConnection() {
    bank::testing::ensureDatabase();
    return "DRIVER=SQLite3;Database=" +
           (std::filesystem::temp_directory_path() / "morph_bank_tests.db").string();
}

}  // namespace

TEST_CASE("AuthModel register/login/change-password flow", "[auth]") {
    bank::app::App app{testConnection()};
    morph::bridge::BridgeHandler<bank::AuthModel> auth{app.bridge(), app.gui()};

    const std::string user = "carol-" + std::to_string(std::filesystem::hash_value("carol"));

    SECTION("registering then logging in succeeds; wrong password fails") {
        auto reg = await(auth.execute(bank::dto::RegisterUser{.username = user,
                                                              .password = "hunter2",
                                                              .displayName = "Carol"}),
                         app.guiLoop());
        REQUIRE(reg.ok);
        REQUIRE(reg.principal == user);
        REQUIRE(reg.displayName == "Carol");

        auto good = await(auth.execute(bank::dto::LoginRequest{.username = user, .password = "hunter2"}),
                          app.guiLoop());
        REQUIRE(good.ok);
        REQUIRE(good.principal == user);

        auto bad = await(auth.execute(bank::dto::LoginRequest{.username = user, .password = "wrong"}),
                         app.guiLoop());
        REQUIRE_FALSE(bad.ok);
    }

    SECTION("registering a duplicate username is rejected") {
        const std::string dupe = user + "-dupe";
        await(auth.execute(bank::dto::RegisterUser{.username = dupe, .password = "pass1"}), app.guiLoop());
        auto second =
            await(auth.execute(bank::dto::RegisterUser{.username = dupe, .password = "pass2"}), app.guiLoop());
        REQUIRE_FALSE(second.ok);
    }

    SECTION("a short password fails validation") {
        REQUIRE_THROWS_AS(
            await(auth.execute(bank::dto::RegisterUser{.username = user + "-x", .password = "no"}),
                  app.guiLoop()),
            bank::ValidationError);
    }

    SECTION("changing the password requires the old one") {
        const std::string cpUser = user + "-cp";
        await(auth.execute(bank::dto::RegisterUser{.username = cpUser, .password = "first"}), app.guiLoop());

        REQUIRE_THROWS_AS(await(auth.execute(bank::dto::ChangePassword{.username = cpUser,
                                                                       .oldPassword = "nope",
                                                                       .newPassword = "second"}),
                                app.guiLoop()),
                          bank::Unauthorized);

        auto ok = await(auth.execute(bank::dto::ChangePassword{.username = cpUser,
                                                               .oldPassword = "first",
                                                               .newPassword = "second"}),
                        app.guiLoop());
        REQUIRE(ok.ok);

        auto login =
            await(auth.execute(bank::dto::LoginRequest{.username = cpUser, .password = "second"}), app.guiLoop());
        REQUIRE(login.ok);
    }
}

TEST_CASE("AuthModel WhoAmI reflects the bridge session", "[auth]") {
    bank::app::App app{testConnection()};
    morph::bridge::BridgeHandler<bank::AuthModel> auth{app.bridge(), app.gui()};

    auto anon = await(auth.execute(bank::dto::WhoAmI{}), app.guiLoop());
    REQUIRE_FALSE(anon.authenticated);

    app.login("dave-session");
    auto known = await(auth.execute(bank::dto::WhoAmI{}), app.guiLoop());
    REQUIRE(known.authenticated);
    REQUIRE(known.principal == "dave-session");
}

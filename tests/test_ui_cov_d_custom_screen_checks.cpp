// SPDX-License-Identifier: Apache-2.0

// The base rule of a custom screen over every command the vocabulary has: the ones that need a capability the base
// lacks are refused by name, and the others never are.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <memory>
#include <morph/session/session.hpp>
#include <morph/ui/custom_screens.hpp>
#include <morph/ui/document.hpp>
#include <optional>
#include <string>
#include <string_view>

#include "ui_custom_screens_support.hpp"

using screens_test::baseScreen;
using screens_test::replaced;

namespace {

struct Rig {
    std::shared_ptr<morph::ui::ServedBundle const> bundle = screens_test::bundle();
    std::shared_ptr<uiservice::RuleAuthorizer> authorizer = std::make_shared<uiservice::RuleAuthorizer>();
    morph::session::Context alice = [] {
        morph::session::Context made;
        made.principal = "alice";
        return made;
    }();

    // The base screen with its Print button's command replaced, and a state the commands may name.
    std::optional<morph::ui::Refusal> check(std::string_view command) const {
        std::string document = replaced(baseScreen("custom:mine"), R"({"host":"print"})", std::string{command});
        document = replaced(document, R"("models":)", R"("state":{"s":{"init":0}},"models":)");
        return morph::ui::checkCustomScreen(*bundle, alice, *authorizer, "lims.base", document, "custom:mine", {},
                                            nullptr);
    }
};

}  // namespace

TEST_CASE("covd custom screen checks: a command the base lacks is refused by its capability",
          "[ui][custom-screens][checks][covd]") {
    struct Case {
        std::string_view command;
        std::string_view capability;
    };
    auto const [command, capability] =
        GENERATE(Case{R"({"download":{"ref":"event"},"save":"dialog"})", "download"},
                 Case{R"({"switchBackend":"local"})", "switchBackend"}, Case{R"({"resetPrefs":"all"})", "resetPrefs"},
                 Case{R"({"signOut":true})", "signOut"}, Case{R"({"signIn":{"from":"save"}})", "signIn"});
    CAPTURE(command);
    Rig const rig;
    auto const refusal = rig.check(command);
    REQUIRE(refusal);
    CHECK(refusal->reason.contains(capability));
}

TEST_CASE("covd custom screen checks: a command that needs no capability passes the base rule",
          "[ui][custom-screens][checks][covd]") {
    auto const command = GENERATE(as<std::string_view>{}, R"({"reset":"s"})", R"({"set":"s","value":{"lit":1}})",
                                  R"({"refetch":"list"})", R"({"setLocale":"de"})", R"({"setContext":{"lit":1}})",
                                  R"({"notify":"saved","role":"info"})", R"({"seq":[{"reset":"s"}]})",
                                  R"({"all":[{"reset":"s"}]})", R"({"if":[{"lit":true},[{"reset":"s"}]]})");
    CAPTURE(command);
    Rig const rig;
    auto const refusal = rig.check(command);
    CHECK_FALSE(refusal);
}

TEST_CASE("covd custom screen checks: the allowance describes a base's params, actions, hosts and commands",
          "[ui][custom-screens][checks][covd]") {
    std::string const base = replaced(baseScreen("lims.base"), R"("models":)", R"("params":{"id":"int"},"models":)");
    auto const described = morph::ui::describeAllowance(screens_test::load(base), base);
    REQUIRE(described.has_value());
    CHECK(described->contains("Params, which a custom screen declares exactly:"));
    CHECK(described->contains("`id`: \"int\""));
    CHECK_FALSE(described->contains("Params: none"));
    CHECK(described->contains("Lims_Sample/Lims_Verify"));
    CHECK(described->contains("host `print`"));
    CHECK(described->contains("upload"));
    CHECK(described->contains("Delivered components:"));

    std::string const plain = baseScreen("lims.base");
    auto const none = morph::ui::describeAllowance(screens_test::load(plain), plain);
    REQUIRE(none.has_value());
    CHECK(none->contains("Params: none; a custom screen declares exactly its base's."));
}

TEST_CASE("covd custom screen checks: the allowance of text that is no JSON is refused",
          "[ui][custom-screens][checks][covd]") {
    auto const described = morph::ui::describeAllowance(screens_test::load(baseScreen("lims.base")), "{not json");
    REQUIRE_FALSE(described.has_value());
    CHECK(described.error().reason.contains("JSON text"));
}

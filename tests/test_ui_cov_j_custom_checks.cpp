// SPDX-License-Identifier: Apache-2.0

// The base rule over commands and places the base screen of the fixtures does not use itself: a screen drop zone's
// file, commands that need no capability, and a mutation run twice from one trigger.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <morph/ui/custom_screens.hpp>
#include <optional>
#include <string>
#include <string_view>

#include "ui_custom_screens_support.hpp"

using screens_test::baseScreen;
using screens_test::load;
using screens_test::replaced;

namespace {

std::optional<morph::ui::Refusal> between(std::string const& base, std::string const& custom) {
    return morph::ui::checkBaseRule(load(base), base, load(custom), custom);
}

std::optional<morph::ui::Refusal> rule(std::string const& custom) { return between(baseScreen("lims.base"), custom); }

constexpr std::string_view kResults = R"({"kind":"text","text":"Results"},)";

}  // namespace

TEST_CASE("covj base rule: commands that need no capability pass wherever the screen puts them",
          "[covj][ui][custom-screens][base-rule]") {
    auto const command = GENERATE(as<std::string_view>{}, R"({"open":"dlg"})", R"({"close":"dlg"})",
                                  R"({"return":"x"})", R"({"focus":"fld"})");
    CAPTURE(command);
    std::string const extras = std::string{kResults} + R"({"kind":"textInput","id":"fld"},)" +
                               R"({"kind":"dialog","id":"dlg","children":[{"kind":"text","text":"d"}]},)";
    std::string const custom =
        replaced(replaced(baseScreen("custom:mine"), kResults, extras), R"({"host":"print"})", std::string{command});
    CHECK_FALSE(rule(custom));
}

TEST_CASE("covj base rule: a mutation run twice from the same trigger is the base's one capability",
          "[covj][ui][custom-screens][base-rule]") {
    std::string const twice = replaced(baseScreen("custom:mine"), R"("onMount":[{"run":"save"}])",
                                       R"("onMount":[{"run":"save"},{"run":"save"}])");
    CHECK_FALSE(rule(twice));
}

TEST_CASE("covj base rule: files a drop zone takes are the file the person dropped",
          "[covj][ui][custom-screens][base-rule]") {
    auto const zone = [](std::string_view file) {
        return replaced(baseScreen("custom:mine"), kResults,
                        std::string{kResults} + R"({"kind":"dropZone","onDrop":[{"upload":)" + std::string{file} +
                            R"(,"to":"save","as":"ref"}],"children":[{"kind":"text","text":"drop"}]},)");
    };
    SECTION("the dropped file") { CHECK_FALSE(rule(zone(R"({"ref":"event"})"))); }
    SECTION("a literal path") {
        auto const refusal = rule(zone(R"("/tmp/a.csv")"));
        REQUIRE(refusal);
        CHECK(refusal->reason.contains("upload"));
        CHECK(refusal->path.starts_with("/view/children/"));
    }
}

TEST_CASE("covj base rule: a close veto and a retry pass where they are valid",
          "[covj][ui][custom-screens][base-rule]") {
    SECTION("cancelClose in onClose") {
        CHECK_FALSE(rule(
            replaced(baseScreen("custom:mine"), R"("onMount":)", R"("onClose":[{"cancelClose":true}],"onMount":)")));
    }
    SECTION("retry in a boundary's error view") {
        std::string const boundary = std::string{kResults} +
                                     R"({"kind":"boundary","children":[{"kind":"text","text":"b"}],)"
                                     R"("error":[{"kind":"button","label":"Again","onClick":[{"retry":true}]}]},)";
        CHECK_FALSE(rule(replaced(baseScreen("custom:mine"), kResults, boundary)));
    }
}

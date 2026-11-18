// SPDX-License-Identifier: Apache-2.0
//
// The base rule: a custom document may use only what its base screen uses. One case per row of the rule.

#include <catch2/catch_test_macros.hpp>
#include <morph/ui/bundle.hpp>
#include <morph/ui/custom_screens.hpp>
#include <optional>
#include <string>

#include "ui_custom_screens_support.hpp"

using screens_test::baseScreen;
using screens_test::load;
using screens_test::replaced;

namespace {

std::optional<morph::ui::Refusal> rule(std::string const& custom) {
    std::string const base = baseScreen("lims.base");
    return morph::ui::checkBaseRule(load(base), base, load(custom), custom);
}

}  // namespace

TEST_CASE("base rule: a rearranged, trimmed copy of the base passes", "[ui][custom-screens][base-rule]") {
    CHECK_FALSE(rule(baseScreen("custom:mine")));
    std::string const trimmed =
        replaced(baseScreen("custom:mine"), R"({"kind":"button","label":"Print","onClick":[{"host":"print"}]},)", "");
    CHECK_FALSE(rule(replaced(trimmed, R"("text":"Results")", R"("text":"Mine")")));
}

TEST_CASE("base rule: an action the base does not use is refused", "[ui][custom-screens][base-rule]") {
    auto const refusal = rule(replaced(baseScreen("custom:mine"), R"("queries":{)",
                                       R"("queries":{"report":{"model":"sample","action":"Lims_Report"},)"));
    REQUIRE(refusal);
    CHECK(refusal->path == "/queries/report");
    CHECK(refusal->reason.contains("Lims_Report"));
}

TEST_CASE("base rule: a command the base runs from another trigger is refused", "[ui][custom-screens][base-rule]") {
    SECTION("an attended action run on mount") {
        auto const refusal =
            rule(replaced(baseScreen("custom:mine"), R"("onMount":[{"run":"save"}])",
                          R"("onMount":[{"run":"save"},{"run":"verify"}])"));
        REQUIRE(refusal);
        CHECK(refusal->path == "/onMount/1");
        CHECK(refusal->reason.contains("onMount: run Lims_Sample/Lims_Verify"));
    }
    SECTION("a mount run moved to unmount") {
        auto const refusal =
            rule(replaced(baseScreen("custom:mine"), R"("onMount":[{"run":"save"}])", R"("onUnmount":[{"run":"save"}])"));
        REQUIRE(refusal);
        CHECK(refusal->path == "/onUnmount/0");
    }
    SECTION("a run chained after an unattended run") {
        auto const refusal =
            rule(replaced(baseScreen("custom:mine"), R"("save":{"model":"sample","action":"Lims_Save"})",
                          R"("save":{"model":"sample","action":"Lims_Save","onSuccess":[{"run":"verify"}]})"));
        REQUIRE(refusal);
        CHECK(refusal->reason.contains("onMount: run Lims_Sample/Lims_Verify"));
    }
    SECTION("a watch, which runs without a press") {
        auto const refusal = rule(replaced(baseScreen("custom:mine"), R"("onMount":)",
                                           R"("watch":[{"on":{"query":"list"},"do":[{"run":"verify"}]}],"onMount":)"));
        REQUIRE(refusal);
        CHECK(refusal->reason.contains("watch: run Lims_Sample/Lims_Verify"));
    }
}

TEST_CASE("base rule: a client-side command the base does not use is refused", "[ui][custom-screens][base-rule]") {
    auto const refusal = rule(replaced(baseScreen("custom:mine"), R"({"kind":"text","text":"Results"},)",
                                       R"({"kind":"text","text":"Results"},)"
                                       R"({"kind":"button","label":"Out","onClick":[{"signOut":true}]},)"));
    REQUIRE(refusal);
    CHECK(refusal->reason.contains("signOut"));
    // The base's own upload, moved elsewhere, is still the base's.
    CHECK_FALSE(rule(replaced(baseScreen("custom:mine"), R"("label":"Attach")", R"("label":"Attach a file")")));
}

TEST_CASE("base rule: a host name the base does not use is refused", "[ui][custom-screens][base-rule]") {
    SECTION("a host command") {
        auto const refusal =
            rule(replaced(baseScreen("custom:mine"), R"({"host":"print"})", R"({"host":"openPath"})"));
        REQUIRE(refusal);
        CHECK(refusal->reason.contains("host `openPath`"));
    }
    SECTION("a host value") {
        auto const refusal = rule(replaced(baseScreen("custom:mine"), R"("text":"Results")",
                                           R"("text":{"fn":"coalesce","args":[{"host":"sync.count"},"-"]})"));
        REQUIRE(refusal);
        CHECK(refusal->reason.contains("host `sync.count`"));
    }
    SECTION("a host component") {
        auto const refusal =
            rule(replaced(baseScreen("custom:mine"), R"("host":"ResultGrid")", R"("host":"Camera")"));
        REQUIRE(refusal);
        CHECK(refusal->path == "/components/Grid/host");
    }
}

TEST_CASE("base rule: a delivered component the base does not declare is refused", "[ui][custom-screens][base-rule]") {
    auto const refusal = rule(replaced(baseScreen("custom:mine"), morph::ui::contentHash(uiservice::kChart),
                                       morph::ui::contentHash("Rectangle {}")));
    REQUIRE(refusal);
    CHECK(refusal->path == "/components/Chart");
}

TEST_CASE("base rule: params other than the base's are refused", "[ui][custom-screens][base-rule]") {
    auto const refusal =
        rule(replaced(baseScreen("custom:mine"), R"("models":)", R"("params":{"sampleId":"int"},"models":)"));
    REQUIRE(refusal);
    CHECK(refusal->path == "/params");
}

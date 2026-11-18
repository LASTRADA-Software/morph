// SPDX-License-Identifier: Apache-2.0
//
// The base rule: a custom document may use only what its base screen uses. One case per row of the rule.

#include <catch2/catch_test_macros.hpp>
#include <morph/ui/bundle.hpp>
#include <morph/ui/custom_screens.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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
    // The base's own upload, moved into a nested column, is still the base's.
    auto const attach =
        R"({"kind":"button","label":"Attach","onClick":[{"upload":"/tmp/a.csv","to":"save","as":"ref"}]})";
    std::string const moved = replaced(replaced(baseScreen("custom:mine"), "," + std::string{attach}, ""),
                                       R"({"kind":"text","text":"Results"},)",
                                       R"({"kind":"text","text":"Results"},{"kind":"column","children":[)" +
                                           std::string{attach} + "]},");
    CHECK_FALSE(rule(moved));
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

namespace {

std::optional<morph::ui::Refusal> between(std::string const& base, std::string const& custom) {
    return morph::ui::checkBaseRule(load(base), base, load(custom), custom);
}

/// A document with one model, one query and the Chart component, and the caller's members: @p top at the top level,
/// @p children in the view's column and @p fallback as Chart's fallback.
std::string miniScreen(std::string_view id, std::string_view top = {}, std::string_view children = {},
                       std::string_view fallback = R"({"kind":"text","text":"chart"})") {
    return R"({"vocab":"ui/1","id":")" + std::string{id} +
           R"(","models":{"sample":{"type":"Lims_Sample"}},)"
           R"("queries":{"list":{"model":"sample","action":"Lims_List"}},)" +
           std::string{top} + R"("components":{"Chart":{"qml":")" + morph::ui::contentHash(uiservice::kChart) +
           R"(","props":{},"events":{"loaded":"int"},"fallback":)" + std::string{fallback} +
           R"(}},"view":{"kind":"column","children":[{"kind":"text","text":"x"})" +
           (children.empty() ? "" : "," + std::string{children}) + "]}}";
}

}  // namespace

TEST_CASE("base rule: a component's fallback is part of the document", "[ui][custom-screens][base-rule]") {
    auto const forEachWithReport =
        R"({"kind":"column","children":[{"kind":"forEach","rows":{"lit":[]},"key":{"row":"id"},)"
        R"("scope":{"models":{"sample":{"type":"Lims_Sample","instance":"private"}},"mutations":{"rep":{"model":"sample","action":"Lims_Report"}}},)"
        R"("children":[{"kind":"button","label":"R","onClick":[{"run":"rep"}]}]}]})";
    auto const refusal = between(miniScreen("lims.base"), miniScreen("custom:mine", {}, {}, forEachWithReport));
    REQUIRE(refusal);
    CHECK(refusal->path == "/components/Chart/fallback/children/0/scope/mutations/rep");
    CHECK(refusal->reason.contains("Lims_Report"));
}

TEST_CASE("base rule: a mutation resolves its model in the scope that declares it", "[ui][custom-screens][base-rule]") {
    auto const withWatch = [](std::string_view id, std::string_view top, std::string_view inner) {
        return miniScreen(id, top,
                          R"({"kind":"forEach","rows":{"lit":[]},"key":{"row":"id"},"scope":{)" +
                              std::string{inner} + R"(},"children":[{"kind":"text","text":"r"}]})");
    };
    SECTION("an inner alias of the same name does not retype an outer mutation") {
        auto const outer = R"("mutations":{"a":{"model":"sample","action":"Lims_Save"}},)";
        auto const run = R"("watch":[{"on":{"query":"list"},"do":[{"run":"a"}]}])";
        auto const shadow = R"("models":{"sample":{"type":"Lims_Audit","instance":"private"}},)";
        // The base runs `a` from a watch at the top; the custom runs it from a watch under an alias that is Lims_Audit
        // there, but `a` is declared against the outer alias: it is the same run.
        CHECK_FALSE(between(withWatch("lims.base", std::string{outer} + run + ",", ""),
                            withWatch("custom:mine", outer, std::string{shadow} + run)));
    }
    SECTION("a chain follows names in the scope that declares the mutation") {
        // Outer `b` is the base's press-only action, run from a head action; an inner scope shadows `b` and runs the
        // shadow and `a` from a watch. Only the custom's `a` chains to `b`, which is the outer one.
        auto const screen = [&](std::string_view id, std::string_view onSuccess) {
            return withWatch(
                id,
                R"("actions":[{"id":"v","label":"V","onClick":[{"run":"b"}]}],)"
                R"("mutations":{"a":{"model":"sample","action":"Lims_Save")" +
                    std::string{onSuccess} + R"(},"b":{"model":"sample","action":"Lims_Verify"}},)",
                R"("mutations":{"b":{"model":"sample","action":"Lims_Report"}},)"
                R"("watch":[{"on":{"query":"list"},"do":[{"run":"b"},{"run":"a"}]}])");
        };
        auto const refusal = between(screen("lims.base", ""), screen("custom:mine", R"(,"onSuccess":[{"run":"b"}])"));
        REQUIRE(refusal);
        CHECK(refusal->reason.contains("watch: run Lims_Sample/Lims_Verify"));
    }
}

TEST_CASE("base rule: an upload or a sign-in without a press runs the mutation it names",
          "[ui][custom-screens][base-rule]") {
    auto const mount = [](std::string_view onMount, std::string_view saveSuccess = {}) {
        return replaced(replaced(baseScreen("custom:mine"), R"("onMount":[{"run":"save"}])",
                                 R"("onMount":)" + std::string{onMount}),
                        R"("save":{"model":"sample","action":"Lims_Save"})",
                        R"("save":{"model":"sample","action":"Lims_Save")" + std::string{saveSuccess} + "}");
    };
    auto const upload = [](std::string_view to) {
        return R"([{"upload":"/tmp/a.csv","to":")" + std::string{to} + R"(","as":"ref"}])";
    };
    std::string const base = mount(upload("save"));
    SECTION("an upload on mount to another mutation") {
        auto const refusal = between(base, mount(upload("verify")));
        REQUIRE(refusal);
        CHECK(refusal->reason.contains("onMount: run Lims_Sample/Lims_Verify"));
    }
    SECTION("an upload's mutation chains to another") {
        auto const refusal = between(base, mount(upload("save"), R"(,"onSuccess":[{"run":"verify"}])"));
        REQUIRE(refusal);
        CHECK(refusal->reason.contains("onMount: run Lims_Sample/Lims_Verify"));
    }
    SECTION("a sign-in's mutation chains to another") {
        auto const login = [](std::string_view onSuccess) {
            return R"({"vocab":"ui/1","id":"lims.login","models":{"auth":{"type":"Auth_Session"},)"
                   R"("sample":{"type":"Lims_Sample"}},)"
                   R"("mutations":{"login":{"model":"auth","action":"Auth_Login")" +
                   std::string{onSuccess} +
                   R"(},"verify":{"model":"sample","action":"Lims_Verify"}},)"
                   R"("onMount":[{"signIn":{"from":"login"}}],)"
                   R"("view":{"kind":"button","label":"V","onClick":[{"run":"verify"}]}})";
        };
        auto const refusal = between(login(""), login(R"(,"onSuccess":[{"run":"verify"}])"));
        REQUIRE(refusal);
        CHECK(refusal->reason.contains("onMount: run Lims_Sample/Lims_Verify"));
    }
}

TEST_CASE("base rule: an action keeps the kind of position it has in the base", "[ui][custom-screens][base-rule]") {
    auto const withForm = [](std::string_view id, std::string_view submit) {
        return miniScreen(id, {},
                          R"({"kind":"form","model":"sample","action":"Lims_Report")" + std::string{submit} + "}");
    };
    SECTION("a mutation used as a query") {
        auto const refusal =
            rule(replaced(baseScreen("custom:mine"), R"("queries":{)",
                          R"("queries":{"again":{"model":"sample","action":"Lims_Save"},)"));
        REQUIRE(refusal);
        CHECK(refusal->path == "/queries/again");
        CHECK(refusal->reason.contains("as a query"));
    }
    SECTION("a form the person submits made one that submits itself") {
        auto const refusal = between(withForm("lims.base", ""), withForm("custom:mine", R"(,"submit":"auto")"));
        REQUIRE(refusal);
        CHECK(refusal->reason.contains("as a auto form"));
    }
    SECTION("the same form stays") {
        CHECK_FALSE(between(withForm("lims.base", ""), withForm("custom:mine", R"(,"submit":"explicit")")));
    }
}

TEST_CASE("base rule: a component's events run without a press", "[ui][custom-screens][base-rule]") {
    auto const withChart = [](std::string_view id, std::string_view onLoaded) {
        return miniScreen(id,
                          R"("mutations":{"verify":{"model":"sample","action":"Lims_Verify"}},)",
                          R"({"kind":"custom","name":"Chart","on":{"loaded":)" + std::string{onLoaded} +
                              R"(}},{"kind":"button","label":"V","onClick":[{"run":"verify"}]})");
    };
    auto const refusal =
        between(withChart("lims.base", R"([{"refetch":"list"}])"), withChart("custom:mine", R"([{"run":"verify"}])"));
    REQUIRE(refusal);
    CHECK(refusal->reason.contains("component:Chart.loaded: run Lims_Sample/Lims_Verify"));
    CHECK_FALSE(between(withChart("lims.base", R"([{"run":"verify"}])"),
                        withChart("custom:mine", R"([{"run":"verify"}])")));
}

// A catalog that knows the action's owner refuses the second alias at load, so the model type is compared only
// where the catalog does not answer actions: the facts here are the model types alone.
TEST_CASE("base rule: the same action on another model type is another action", "[ui][custom-screens][base-rule]") {
    auto facts = uiservice::facts();
    facts.action = nullptr;
    auto const loadWithout = [&](std::string const& text) {
        auto loaded = morph::ui::loadDocumentText(text, morph::ui::LoadOptions{.catalog = facts});
        if (!loaded) {
            FAIL("fixture does not load: " << loaded.error().message());
        }
        return std::move(*loaded);
    };
    std::string const base = miniScreen("lims.base");
    std::string const custom =
        replaced(replaced(miniScreen("custom:mine"), R"("models":{"sample":{"type":"Lims_Sample"}})",
                          R"("models":{"sample":{"type":"Lims_Sample"},"audit":{"type":"Lims_Audit"}})"),
                 R"("queries":{)", R"("queries":{"trail":{"model":"audit","action":"Lims_List"},)");
    auto const refusal = morph::ui::checkBaseRule(loadWithout(base), base, loadWithout(custom), custom);
    REQUIRE(refusal);
    CHECK(refusal->path == "/queries/trail");
    CHECK(refusal->reason.contains("Lims_Audit"));
}

TEST_CASE("base rule: every place a command can sit is walked", "[ui][custom-screens][base-rule]") {
    std::string const out = R"({"signOut":true})";
    struct Place {
        std::string name;
        std::string top;
        std::string children;
        std::string fallback;
    };
    auto const press = [&](std::string const& command) {
        return R"({"kind":"button","label":"B","onClick":[)" + command + "]}";
    };
    std::vector<Place> const places = {
        {"document keys", R"("keys":{"Ctrl+Shift+Y":[)" + out + R"(]},)", "", ""},
        {"node keys", "", R"({"kind":"text","text":"k","keys":{"Enter":[)" + out + "]}}", ""},
        {"head actions", R"("actions":[{"id":"a","label":"A","onClick":[)" + out + R"(]}],)", "", ""},
        {"head menu", R"("menu":[{"id":"m","label":"M","onClick":[)" + out + R"(]}],)", "", ""},
        {"head tools", R"("tools":[{"id":"t","kind":"search","label":"T","onChange":[)" + out + R"(]}],)", "", ""},
        {"component fallback", "", "", press(out)},
        {"watch in a nested scope", "",
         R"({"kind":"forEach","rows":{"lit":[]},"key":{"row":"id"},"scope":{"watch":[{"on":{"query":"list"},)"
         R"("do":[)" + out + R"(]}]},"children":[{"kind":"text","text":"r"}]})",
         ""},
        {"seq", "", press(R"({"seq":[)" + out + "]}"), ""},
        {"all", "", press(R"({"all":[)" + out + "]}"), ""},
        {"if, else branch", "", press(R"({"if":[{"lit":true},[],[)" + out + "]]}"), ""},
        {"navigate, onResult", "", press(R"({"navigate":"lims.other","onResult":[)" + out + "]}"), ""},
        {"navigate, onCancel", "", press(R"({"navigate":"lims.other","onCancel":[)" + out + "]}"), ""},
    };
    for (Place const& place : places) {
        DYNAMIC_SECTION(place.name) {
            auto const refusal = between(
                miniScreen("lims.base"),
                miniScreen("custom:mine", place.top, place.children,
                           place.fallback.empty() ? R"({"kind":"text","text":"chart"})" : place.fallback));
            REQUIRE(refusal);
            CHECK(refusal->reason.contains("signOut"));
        }
    }
}

TEST_CASE("base rule: a form that submits itself runs its chains without a press", "[ui][custom-screens][base-rule]") {
    auto const screen = [](std::string_view id, std::string_view onSuccess) {
        return miniScreen(id, R"("mutations":{"verify":{"model":"sample","action":"Lims_Verify"}},)",
                          R"({"kind":"form","model":"sample","action":"Lims_Report","submit":"auto","onSuccess":)" +
                              std::string{onSuccess} +
                              R"(},{"kind":"button","label":"V","onClick":[{"run":"verify"}]})");
    };
    auto const refusal = between(screen("lims.base", R"([{"refetch":"list"}])"),
                                 screen("custom:mine", R"([{"run":"verify"}])"));
    REQUIRE(refusal);
    CHECK(refusal->reason.contains("form-auto:Lims_Sample/Lims_Report: run Lims_Sample/Lims_Verify"));
    CHECK_FALSE(between(screen("lims.base", R"([{"refetch":"list"}])"), screen("custom:mine", R"([{"refetch":"list"}])")));
}

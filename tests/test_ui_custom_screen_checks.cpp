// SPDX-License-Identifier: Apache-2.0
//
// The checks a custom document passes on save and on every load: one case per check, in their order, the id
// rewrite, and the verdict cache.

#include <catch2/catch_test_macros.hpp>
#include <morph/session/session.hpp>
#include <morph/ui/author.hpp>
#include <morph/ui/custom_screens.hpp>
#include <morph/ui/document.hpp>
#include <optional>
#include <string>
#include <vector>

#include "ui_custom_screens_support.hpp"

using screens_test::baseScreen;
using screens_test::replaced;

namespace {

struct Fixture {
    std::shared_ptr<morph::ui::ServedBundle const> bundle = screens_test::bundle();
    std::shared_ptr<uiservice::RuleAuthorizer> authorizer = std::make_shared<uiservice::RuleAuthorizer>();
    morph::session::Context alice = [] {
        morph::session::Context made;
        made.principal = "alice";
        return made;
    }();

    std::optional<morph::ui::Refusal> check(std::string const& document, std::string const& basedOn = "lims.base",
                                            morph::ui::CustomScreensOptions options = {},
                                            morph::ui::VerdictCache* cache = nullptr) const {
        return morph::ui::checkCustomScreen(*bundle, alice, *authorizer, basedOn, document, options, cache);
    }
};

}  // namespace

TEST_CASE("checks: a copy of the base passes", "[ui][custom-screens][checks]") {
    Fixture const f;
    CHECK_FALSE(f.check(baseScreen("lims.base")));
}

TEST_CASE("checks: 1, the bounds", "[ui][custom-screens][checks]") {
    Fixture const f;
    auto const large = f.check(baseScreen("lims.base"), "lims.base", morph::ui::CustomScreensOptions{.maxDocumentBytes = 64});
    REQUIRE(large);
    CHECK(large->reason.contains("longer than 64 bytes"));
    auto const deep = f.check(std::string(65, '[') + std::string(65, ']'));
    REQUIRE(deep);
    CHECK(deep->reason.contains("nests deeper"));
}

TEST_CASE("checks: 2, the base", "[ui][custom-screens][checks]") {
    Fixture f;
    auto const unknown = f.check(baseScreen("lims.base"), "lims.nothing");
    REQUIRE(unknown);
    CHECK(unknown->reason.contains("names no screen document"));
    auto const host = f.check(baseScreen("lims.base"), "lims.scanner");
    REQUIRE(host);
    CHECK(host->reason.contains("names no screen document"));
    f.authorizer->denied["alice"] = {{"Lims_Sample", "Lims_List"}};
    auto const closed = f.check(baseScreen("lims.base"));
    REQUIRE(closed);
    CHECK(closed->reason.contains("may not open"));
}

TEST_CASE("checks: 3, the application screen checks", "[ui][custom-screens][checks]") {
    Fixture const f;
    std::string const broken = replaced(baseScreen("lims.base"), R"({"kind":"text","text":"Results"})",
                                        R"({"kind":"nope","text":"Results"})");
    auto const expected = morph::ui::loadDocumentText(broken, morph::ui::LoadOptions{.catalog = f.bundle->catalog});
    REQUIRE_FALSE(expected);
    auto const refusal = f.check(broken);
    REQUIRE(refusal);
    CHECK(refusal->path == expected.error().path);
    CHECK(refusal->reason == expected.error().reason);

    std::string const app = R"({"vocab":"ui/1","id":"app","models":{"sample":{"type":"Lims_Sample"}},)"
                            R"("queries":{"list":{"model":"sample","action":"Lims_List"}}})";
    auto const appRefusal = f.check(app);
    REQUIRE(appRefusal);
    CHECK(appRefusal->reason.contains("not the app document"));
}

TEST_CASE("checks: 4, the accessibility rules", "[ui][custom-screens][checks]") {
    Fixture const f;
    std::string const unlabelled = replaced(baseScreen("lims.base"), R"("label":"Verify")", R"("label":"")");
    auto const expected = morph::ui::author::checkAccessibility(screens_test::load(unlabelled));
    REQUIRE(expected);
    auto const refusal = f.check(unlabelled);
    REQUIRE(refusal);
    CHECK(refusal->path == expected->path);
}

TEST_CASE("checks: 5, the base rule", "[ui][custom-screens][checks]") {
    Fixture const f;
    auto const refusal = f.check(replaced(baseScreen("lims.base"), R"({"host":"print"})", R"({"signOut":true})"));
    REQUIRE(refusal);
    CHECK(refusal->reason.contains("signOut"));
}

TEST_CASE("checks: 6, permission", "[ui][custom-screens][checks]") {
    Fixture f;
    f.authorizer->denied["alice"] = {{"Lims_Sample", "Lims_Verify"}};
    auto const refusal = f.check(baseScreen("lims.base"));
    REQUIRE(refusal);
    CHECK(refusal->path == "/mutations/verify");
    CHECK(refusal->reason.contains("may not run action `Lims_Verify`"));
}

TEST_CASE("withScreenId puts the custom screen's id on the document", "[ui][custom-screens][checks]") {
    auto const rewritten = morph::ui::withScreenId(baseScreen("lims.results"), "custom:0123");
    REQUIRE(rewritten);
    CHECK(screens_test::load(*rewritten).id == "custom:0123");
    CHECK_FALSE(morph::ui::withScreenId("{not json", "custom:1"));
    CHECK_FALSE(morph::ui::withScreenId("[1,2]", "custom:1"));
}

TEST_CASE("VerdictCache is bounded and drops the least recently used", "[ui][custom-screens][cache]") {
    morph::ui::VerdictCache cache{2};
    cache.store("a", std::nullopt);
    cache.store("b", morph::ui::Refusal{.document = {}, .path = "/x", .reason = "no"});
    REQUIRE(cache.find("a").has_value());  // a is now the most recent
    cache.store("c", std::nullopt);
    CHECK(cache.size() == 2);
    CHECK_FALSE(cache.find("b").has_value());
    auto const a = cache.find("a");
    REQUIRE(a.has_value());
    CHECK_FALSE(a->has_value());
}

TEST_CASE("the verdict key changes with the document, the base, the bundle and the permitted set",
          "[ui][custom-screens][cache]") {
    std::vector<std::string> const one{"Lims_List"};
    std::vector<std::string> const two{"Lims_List", "Lims_Save"};
    std::string const key = morph::ui::verdictKey("lims.base", "{}", "sha256:aa", one);
    CHECK(key == morph::ui::verdictKey("lims.base", "{}", "sha256:aa", one));
    CHECK(key != morph::ui::verdictKey("lims.base", "{ }", "sha256:aa", one));
    CHECK(key != morph::ui::verdictKey("lims.other", "{}", "sha256:aa", one));
    CHECK(key != morph::ui::verdictKey("lims.base", "{}", "sha256:bb", one));
    CHECK(key != morph::ui::verdictKey("lims.base", "{}", "sha256:aa", two));
}

TEST_CASE("checkCustomScreen reads and fills the verdict cache", "[ui][custom-screens][cache]") {
    Fixture f;
    morph::ui::VerdictCache cache{8};
    std::string const document = baseScreen("lims.base");
    CHECK_FALSE(f.check(document, "lims.base", {}, &cache));
    CHECK(cache.size() == 1);
    CHECK_FALSE(f.check(document, "lims.base", {}, &cache));
    CHECK(cache.size() == 1);

    // A planted verdict is answered, so the lookup is really used.
    auto const permitted = morph::ui::permittedActions(*f.bundle, f.alice, *f.authorizer);
    cache.store(morph::ui::verdictKey("lims.base", document, f.bundle->digest, permitted),
                morph::ui::Refusal{.document = {}, .path = "/planted", .reason = "planted"});
    auto const planted = f.check(document, "lims.base", {}, &cache);
    REQUIRE(planted);
    CHECK(planted->path == "/planted");

    // A permission change misses the cache and checks again.
    f.authorizer->denied["alice"] = {{"Lims_Sample", "Lims_Verify"}};
    auto const denied = f.check(document, "lims.base", {}, &cache);
    REQUIRE(denied);
    CHECK(denied->path == "/mutations/verify");
}

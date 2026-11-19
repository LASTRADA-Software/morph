// SPDX-License-Identifier: Apache-2.0
//
// The checks a custom document passes on save and on every load: one case per check, in their order, the id
// rewrite, and the verdict cache.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <morph/session/session.hpp>
#include <morph/ui/author.hpp>
#include <morph/ui/custom_screens.hpp>
#include <morph/ui/document.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
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

    /// The `id` the document names, which the checks want it stored under.
    static std::string idOf(std::string const& document) {
        auto const parsed = morph::ui::parseJson(document);
        auto const* const object = parsed ? std::get_if<morph::ui::Json::object_t>(&parsed->data) : nullptr;
        auto const found = object == nullptr ? morph::ui::Json::object_t::const_iterator{} : object->find("id");
        auto const* const text =
            object == nullptr || found == object->end() ? nullptr : std::get_if<std::string>(&found->second.data);
        return text == nullptr ? std::string{} : *text;
    }

    std::optional<morph::ui::Refusal> check(std::string const& document, std::string const& basedOn = "lims.base",
                                            morph::ui::CustomScreensOptions options = {},
                                            morph::ui::VerdictCache* cache = nullptr) const {
        return morph::ui::checkCustomScreen(*bundle, alice, *authorizer, basedOn, document, idOf(document), options,
                                            cache);
    }
};

}  // namespace

TEST_CASE("checks: a copy of the base passes", "[ui][custom-screens][checks]") {
    Fixture const f;
    CHECK_FALSE(f.check(baseScreen("custom:mine")));
}

TEST_CASE("checks: 1, the bounds", "[ui][custom-screens][checks]") {
    Fixture const f;
    auto const large =
        f.check(baseScreen("custom:mine"), "lims.base", morph::ui::CustomScreensOptions{.maxDocumentBytes = 64});
    REQUIRE(large);
    CHECK(large->reason.contains("longer than 64 bytes"));
    // An unknown base: only the bounds, which run before the base, can answer with this wording.
    auto const deep = f.check(std::string(65, '[') + std::string(65, ']'), "lims.nothing");
    REQUIRE(deep);
    CHECK(deep->reason.contains("a custom screen nests deeper"));
}

TEST_CASE("checks: 2, the base", "[ui][custom-screens][checks]") {
    Fixture const f;
    auto const unknown = f.check(baseScreen("custom:mine"), "lims.nothing");
    REQUIRE(unknown);
    CHECK(unknown->reason.contains("names no screen document"));
    auto const host = f.check(baseScreen("custom:mine"), "lims.scanner");
    REQUIRE(host);
    CHECK(host->reason.contains("names no screen document"));
    f.authorizer->denied["alice"] = {{"Lims_Sample", "Lims_List"}};
    auto const closed = f.check(baseScreen("custom:mine"));
    REQUIRE(closed);
    CHECK(closed->reason.contains("may not open"));
}

TEST_CASE("checks: 3, the application screen checks", "[ui][custom-screens][checks]") {
    Fixture const f;
    std::string const broken = replaced(baseScreen("custom:mine"), R"({"kind":"text","text":"Results"})",
                                        R"({"kind":"nope","text":"Results"})");
    auto const expected = morph::ui::loadDocumentText(broken, morph::ui::LoadOptions{.catalog = f.bundle->catalog});
    REQUIRE_FALSE(expected);
    auto const refusal = f.check(broken);
    REQUIRE(refusal);
    CHECK(refusal->path == expected.error().path);
    CHECK(refusal->reason == expected.error().reason);

    // The app document is no custom screen: its id is `app`, which a custom screen's id never is.
    std::string const app = R"({"vocab":"ui/1","id":"app","models":{"sample":{"type":"Lims_Sample"}},)"
                            R"("queries":{"list":{"model":"sample","action":"Lims_List"}}})";
    auto const appRefusal = f.check(app);
    REQUIRE(appRefusal);
    CHECK(appRefusal->path == "/id");
    CHECK(appRefusal->reason.contains("starts with `custom:`"));
}

TEST_CASE("checks: 4, the accessibility rules", "[ui][custom-screens][checks]") {
    Fixture const f;
    std::string const unlabelled = replaced(baseScreen("custom:mine"), R"("label":"Verify")", R"("label":"")");
    auto const expected = morph::ui::author::checkAccessibility(screens_test::load(unlabelled));
    REQUIRE(expected);
    auto const refusal = f.check(unlabelled);
    REQUIRE(refusal);
    CHECK(refusal->path == expected->path);
}

TEST_CASE("checks: 5, the base rule", "[ui][custom-screens][checks]") {
    Fixture const f;
    auto const refusal = f.check(replaced(baseScreen("custom:mine"), R"({"host":"print"})", R"({"signOut":true})"));
    REQUIRE(refusal);
    CHECK(refusal->reason.contains("signOut"));
}

TEST_CASE("checks: 6, permission", "[ui][custom-screens][checks]") {
    Fixture const f;
    f.authorizer->denied["alice"] = {{"Lims_Sample", "Lims_Verify"}};
    auto const refusal = f.check(baseScreen("custom:mine"));
    REQUIRE(refusal);
    CHECK(refusal->path == "/mutations/verify");
    CHECK(refusal->reason.contains("may not run action `Lims_Verify`"));
}

TEST_CASE("checks: the first failing check answers", "[ui][custom-screens][checks]") {
    Fixture const f;
    f.authorizer->denied["alice"] = {{"Lims_Sample", "Lims_Verify"}};
    std::string const unlabelled = replaced(baseScreen("custom:mine"), R"("label":"Verify")", R"("label":"")");

    // 3 before 4: a document that does not load is not checked for labels.
    std::string const both =
        replaced(unlabelled, R"({"kind":"text","text":"Results"})", R"({"kind":"nope","text":"Results"})");
    auto const loadRefusal = morph::ui::loadDocumentText(both, morph::ui::LoadOptions{.catalog = f.bundle->catalog});
    REQUIRE_FALSE(loadRefusal);
    auto const loadFirst = f.check(both);
    REQUIRE(loadFirst);
    CHECK(loadFirst->reason == loadRefusal.error().reason);

    // 4 before 6: an unlabelled button and a denied action.
    auto const label = f.check(unlabelled);
    REQUIRE(label);
    CHECK(label->path != "/mutations/verify");
    CHECK_FALSE(label->reason.contains("may not run action"));

    // 5 before 6: a command the base lacks and a denied action.
    auto const rule = f.check(replaced(baseScreen("custom:mine"), R"({"host":"print"})", R"({"signOut":true})"));
    REQUIRE(rule);
    CHECK(rule->reason.contains("signOut"));
    CHECK_FALSE(rule->reason.contains("may not run action"));
}

TEST_CASE("checks: the base is read on every call, past the verdict cache", "[ui][custom-screens][cache]") {
    Fixture f;
    morph::ui::VerdictCache cache{8};
    std::string const document = baseScreen("custom:mine");
    CHECK_FALSE(f.check(document, "lims.base", {}, &cache));

    // The same bundle but for the base entry's `available`, which the manifest does not carry.
    morph::ui::UiRegistry made = uiservice::registry();
    morph::ui::CatalogEntry entry = uiservice::entry("lims.base");
    entry.available = false;
    entry.reason = "withdrawn";
    auto const added = made.addScreen(entry, baseScreen("lims.base"));
    REQUIRE(added);
    auto const rebuilt = made.build();
    REQUIRE(rebuilt);
    REQUIRE((*rebuilt)->digest == f.bundle->digest);
    f.bundle = *rebuilt;
    auto const refusal = f.check(document, "lims.base", {}, &cache);
    REQUIRE(refusal);
    CHECK(refusal->reason.contains("may not open"));
}

TEST_CASE("checkCustomScreen takes a permitted set computed once", "[ui][custom-screens][checks]") {
    Fixture const f;
    auto const permitted = morph::ui::permittedActions(*f.bundle, f.alice, *f.authorizer);
    CHECK_FALSE(morph::ui::checkCustomScreen(*f.bundle, f.alice, *f.authorizer, permitted, "lims.base",
                                             baseScreen("custom:mine"), "custom:mine", {}));
    auto const none = std::vector<std::string>{};
    auto const refusal = morph::ui::checkCustomScreen(*f.bundle, f.alice, *f.authorizer, none, "lims.base",
                                                      baseScreen("custom:mine"), "custom:mine", {});
    REQUIRE(refusal);
    CHECK(refusal->reason.contains("may not run action"));
}

TEST_CASE("withScreenId puts the custom screen's id on the document", "[ui][custom-screens][checks]") {
    auto const rewritten = morph::ui::withScreenId(baseScreen("lims.results"), "custom:0123");
    REQUIRE(rewritten);
    CHECK(screens_test::load(*rewritten).id == "custom:0123");
    auto const added = morph::ui::withScreenId(R"({"vocab":"ui/1","view":{"kind":"text","text":"a"}})", "custom:2");
    REQUIRE(added);
    CHECK(added->contains(R"("id":"custom:2")"));
    auto const overwritten = morph::ui::withScreenId(R"({"id":5,"view":{"kind":"text","text":"a"}})", "custom:3");
    REQUIRE(overwritten);
    CHECK(overwritten->contains(R"("id":"custom:3")"));
    CHECK_FALSE(overwritten->contains("5"));
    auto const nested = morph::ui::withScreenId(R"({"view":{"id":"keep","kind":"text"}})", "custom:4");
    REQUIRE(nested);
    CHECK(nested->contains(R"("id":"keep")"));
    CHECK(nested->contains(R"("id":"custom:4")"));
    auto const duplicated = morph::ui::withScreenId(R"({"id":"a","id":"b","vocab":"ui/1"})", "custom:5");
    REQUIRE(duplicated);
    CHECK(duplicated->find(R"("id")") == duplicated->rfind(R"("id")"));
    CHECK(duplicated->contains(R"("id":"custom:5")"));
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
    std::string const key = morph::ui::verdictKey("lims.base", "{}", "custom:1", "sha256:aa", one);
    CHECK(key == morph::ui::verdictKey("lims.base", "{}", "custom:1", "sha256:aa", one));
    CHECK(key != morph::ui::verdictKey("lims.base", "{ }", "custom:1", "sha256:aa", one));
    CHECK(key != morph::ui::verdictKey("lims.other", "{}", "custom:1", "sha256:aa", one));
    CHECK(key != morph::ui::verdictKey("lims.base", "{}", "custom:1", "sha256:bb", one));
    CHECK(key != morph::ui::verdictKey("lims.base", "{}", "custom:1", "sha256:aa", two));
    CHECK(key != morph::ui::verdictKey("lims.base", "{}", "custom:2", "sha256:aa", one));
}

TEST_CASE("checkCustomScreen reads and fills the verdict cache", "[ui][custom-screens][cache]") {
    Fixture f;
    morph::ui::VerdictCache cache{8};
    std::string const document = baseScreen("custom:mine");
    CHECK_FALSE(f.check(document, "lims.base", {}, &cache));
    CHECK(cache.size() == 1);
    CHECK_FALSE(f.check(document, "lims.base", {}, &cache));
    CHECK(cache.size() == 1);

    // A planted verdict is answered, so the lookup is really used.
    auto const permitted = morph::ui::permittedActions(*f.bundle, f.alice, *f.authorizer);
    cache.store(morph::ui::verdictKey("lims.base", document, Fixture::idOf(document), f.bundle->digest, permitted),
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

TEST_CASE("withScreenId keeps a control byte after an escaped character", "[ui][custom-screens][checks]") {
    // The text is `a`, a backslash, U+000B and `b`. The writer's default leaves U+000B raw, and after the earlier
    // escaped backslash it can write it back as other bytes.
    std::string const text = R"({"vocab":"ui/1","view":{"kind":"text","text":"a\\\u000bb"}})";
    auto const rewritten = morph::ui::withScreenId(text, "custom:6");
    REQUIRE(rewritten);
    CHECK_FALSE(rewritten->contains('\x0b'));
    auto const loaded = morph::ui::loadDocumentText(*rewritten, {});
    REQUIRE(loaded);
    CHECK(loaded->id == "custom:6");
    auto const again = morph::ui::withScreenId(*rewritten, "custom:6");
    REQUIRE(again);
    CHECK(*again == *rewritten);
}

// A command-list key the element table gains must be classified (pressed, a chosen file, a form's outcome, or a
// component's event) before a custom screen can use it, so the base rule never compares an unattended run as a press.
TEST_CASE("checks: every command-list key of the element table is classified", "[ui][custom-screens][checks]") {
    using morph::ui::detail::Shape;
    std::vector<std::pair<std::string_view, std::string_view>> declared;
    for (bool const part : {false, true}) {
        for (std::string_view const kind : part ? morph::ui::partKinds() : morph::ui::nodeKinds()) {
            auto const* const spec = morph::ui::detail::findKind(kind, part);
            REQUIRE(spec != nullptr);
            for (morph::ui::detail::KeySpec const& key : spec->keys) {
                if (key.shape == Shape::Commands || key.shape == Shape::CommandRecord) {
                    declared.emplace_back(kind, key.key);
                }
            }
        }
    }
    REQUIRE_FALSE(declared.empty());
    for (auto const& [kind, key] : declared) {
        INFO("`" << kind << "` declares the command-list key `" << key << "`, which `eventKeys` does not classify");
        CHECK(std::ranges::any_of(morph::ui::detail::eventKeys(), [&](morph::ui::detail::EventKey const& entry) {
            return entry.kind == kind && entry.key == key;
        }));
    }
    for (morph::ui::detail::EventKey const& entry : morph::ui::detail::eventKeys()) {
        INFO("`eventKeys` lists `" << entry.kind << "` `" << entry.key
                                   << "`, which the element table does not declare");
        CHECK(std::ranges::any_of(
            declared, [&](auto const& known) { return known.first == entry.kind && known.second == entry.key; }));
    }
}

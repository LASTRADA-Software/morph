// SPDX-License-Identifier: Apache-2.0
//
// Guided tours in the app document (docs/spec/ui/tours.md, "The declaration"): the loader reads `tours` and refuses a
// malformed one where it is malformed, a screen declares none, `AppRoot::tours` evaluates each text in the app scope
// and again after a change of locale, and `author::withTours` writes a member the loader reads.

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <morph/ui/author.hpp>
#include <morph/ui/document.hpp>
#include <morph/ui/interpreter.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ui_interpreter_support.hpp"

namespace ui = morph::ui;

namespace {

constexpr std::string_view kApp = R"({
  "vocab": "ui/1", "id": "app",
  "tours": [
    { "id": "getting-started",
      "title": { "t": "tours.start.title", "fallback": "Getting started" },
      "summary": "The shell in four steps",
      "steps": [
        { "target": "shell_search", "title": { "t": "tours.start.search", "fallback": "Search" },
          "body": "Find anything." },
        { "target": "nav_samples", "advanceOn": { "screen": "samples" }, "title": "Open Samples" },
        { "screen": "samples", "params": { "page": 2 }, "target": "samples_table", "title": "The table" },
        { "title": "Done", "later": true } ] },
    { "id": "register-sample", "screen": "samples", "title": "Register a sample",
      "steps": [ { "target": "action_new", "title": "New" } ] } ] })";

/// The app document with @p tours as its `tours` member.
std::string appWith(std::string_view tours) {
    return R"({"vocab":"ui/1","id":"app","tours":)" + std::string{tours} + "}";
}

/// @p count steps, each with only a title.
std::string manySteps(std::size_t count) {
    std::string out;
    for (std::size_t index = 0; index < count; ++index) {
        out += index == 0 ? "" : ",";
        out += R"({"title":"S"})";
    }
    return out;
}

}  // namespace

// Mutation: skip `tours` in `DocumentParser::parse` (the document loads with no tours).
TEST_CASE("tours document: the app document's tours load with their steps", "[ui][tours]") {
    auto loaded = ui::loadDocumentText(kApp);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->tours.size() == 2);
    ui::TourDecl const& start = loaded->tours.front();
    CHECK(start.id == "getting-started");
    CHECK(start.path == "/tours/0");
    CHECK(start.screen.empty());
    REQUIRE(start.summary != nullptr);
    REQUIRE(start.steps.size() == 4);
    CHECK(start.steps.at(0).target == "shell_search");
    CHECK(start.steps.at(0).body != nullptr);
    CHECK(start.steps.at(1).advanceOn == "samples");
    CHECK(start.steps.at(2).screen == "samples");
    CHECK(start.steps.at(2).params.at("page") == ui::Value{2});
    // An unknown step member is ignored.
    CHECK(start.steps.at(3).target.empty());
    CHECK(start.steps.at(3).body == nullptr);
    CHECK(loaded->tours.at(1).screen == "samples");
}

// Mutation: drop any one check from `tours`, `tourOf` or `tourStepOf` (its row then loads, or is refused elsewhere).
TEST_CASE("tours document: a malformed tour is refused where it is malformed", "[ui][tours]") {
    struct Row {
        std::string name;
        std::string text;
        std::string path;
        std::string reason;
    };
    std::vector<Row> const rows{
        {"a screen's tours", R"({"vocab":"ui/1","id":"lims.samples","tours":[],"view":{"kind":"text","text":"x"}})",
         "/tours", "the app document's"},
        {"not an array", appWith("{}"), "/tours", "an array"},
        {"an empty id", appWith(R"([{"id":"","title":"T","steps":[{"title":"S"}]}])"), "/tours/0/id", "[a-z0-9-]"},
        {"an upper-case id", appWith(R"([{"id":"Start","title":"T","steps":[{"title":"S"}]}])"), "/tours/0/id",
         "[a-z0-9-]"},
        {"a long id", appWith(R"([{"id":")" + std::string(65, 'a') + R"(","title":"T","steps":[{"title":"S"}]}])"),
         "/tours/0/id", "at most 64"},
        {"a repeated id",
         appWith(R"([{"id":"a","title":"T","steps":[{"title":"S"}]},{"id":"a","title":"U","steps":[{"title":"S"}]}])"),
         "/tours/1/id", "twice"},
        {"no title", appWith(R"([{"id":"a","steps":[{"title":"S"}]}])"), "/tours/0", "`title` is required"},
        {"no steps", appWith(R"([{"id":"a","title":"T","steps":[]}])"), "/tours/0/steps", "one step or more"},
        {"too many steps", appWith(R"([{"id":"a","title":"T","steps":[)" + manySteps(51) + "]}]"), "/tours/0/steps",
         "at most 50"},
        {"no steps member", appWith(R"([{"id":"a","title":"T"}])"), "/tours/0", "`steps` is required"},
        {"steps that are not an array", appWith(R"([{"id":"a","title":"T","steps":{}}])"), "/tours/0/steps",
         "an array of steps"},
        {"a tour that is not a record", appWith(R"(["a"])"), "/tours/0", "a tour is a record"},
        {"params that are not a record", appWith(R"([{"id":"a","title":"T","steps":[{"title":"S","params":[1]}]}])"),
         "/tours/0/steps/0/params", "a record of literal values"},
        {"an advanceOn that is not a record",
         appWith(R"([{"id":"a","title":"T","steps":[{"title":"S","advanceOn":"samples"}]}])"),
         "/tours/0/steps/0/advanceOn", "`advanceOn` is a record"},
        {"a tour-level must-understand member",
         appWith(R"([{"id":"a","title":"T","!pause":1,"steps":[{"title":"S"}]}])"), "/tours/0/!pause",
         "must-understand"},
        {"a step without a title", appWith(R"([{"id":"a","title":"T","steps":[{"body":"B"}]}])"), "/tours/0/steps/0",
         "`title` is required"},
        {"a computed param",
         appWith(R"([{"id":"a","title":"T","steps":[{"title":"S","screen":"x","params":{"p":{"ref":"y"}}}]}])"),
         "/tours/0/steps/0/params/p", "a literal"},
        {"an advanceOn without a screen",
         appWith(R"([{"id":"a","title":"T","steps":[{"title":"S","advanceOn":{}}]}])"), "/tours/0/steps/0/advanceOn",
         "`screen` is required"},
        {"a must-understand member", appWith(R"([{"id":"a","title":"T","steps":[{"title":"S","!pause":1}]}])"),
         "/tours/0/steps/0/!pause", "must-understand"},
        {"a text that names nothing", appWith(R"([{"id":"a","title":{"ref":"nothing"},"steps":[{"title":"S"}]}])"),
         "/tours/0/title", "nothing"},
    };
    for (Row const& row : rows) {
        INFO(row.name);
        auto const loaded = ui::loadDocumentText(row.text);
        REQUIRE_FALSE(loaded.has_value());
        CHECK(loaded.error().path == row.path);
        CHECK(loaded.error().reason.contains(row.reason));
    }
}

// Mutation: refuse an unknown tour-level member in `tourOf` (the tour no longer loads).
TEST_CASE("tours document: an unknown tour-level member is ignored", "[ui][tours]") {
    auto const loaded =
        ui::loadDocumentText(appWith(R"([{"id":"a","title":"T","later":true,"steps":[{"title":"S"}]}])"));
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->tours.size() == 1);
    CHECK(loaded->tours.front().id == "a");
}

// Mutation: evaluate each text once in `mountApp`, outside the computed value (the German title never shows).
TEST_CASE("tours document: AppRoot::tours evaluates the texts in the app scope and again after a change of locale",
          "[ui][tours]") {
    ui::testing::Harness h;
    h.session.setTranslations([](std::string_view key, std::string_view locale) -> std::optional<std::string> {
        if (!locale.starts_with("de")) {
            return std::nullopt;
        }
        if (key == "tours.start.title") {
            return std::string{"Erste Schritte"};
        }
        if (key == "tours.start.search") {
            return std::string{"Suche"};
        }
        return std::nullopt;
    });
    auto root = h.interpreter.mountApp(ui::testing::loadOrFail(kApp));
    REQUIRE(root.has_value());
    std::vector<ui::Tour> tours = (*root)->tours().get();
    REQUIRE(tours.size() == 2);
    CHECK(tours.front().id == "getting-started");
    CHECK(tours.front().title == "Getting started");
    CHECK(tours.front().summary == "The shell in four steps");
    CHECK(tours.front().steps.front().title == "Search");
    CHECK(tours.front().steps.front().body == "Find anything.");
    CHECK(tours.front().steps.at(1).advanceOn == "samples");
    CHECK(tours.front().steps.at(2).params.at("page") == ui::Value{2});
    CHECK(tours.at(1).screen == "samples");
    h.session.setLocale("de");
    h.settle();
    tours = (*root)->tours().get();
    CHECK(tours.front().title == "Erste Schritte");
    CHECK(tours.front().steps.front().title == "Suche");
}

// Mutation: leave `target` out of `author::tourJson` (the loaded steps have none).
TEST_CASE("tours document: author::withTours writes a tours member the loader reads", "[ui][tours][author]") {
    namespace author = ui::author;
    std::vector<author::Tour> const tours{author::Tour{
        .id = "getting-started",
        .screen = {},
        .title = author::t("tours.start.title", "Getting started"),
        .summary = "The shell",
        .steps = {author::TourStep{.title = "Search", .body = "Find anything.", .target = "shell_search"},
                  author::TourStep{.title = "Open Samples", .target = "nav_samples", .advanceOn = "samples"},
                  author::TourStep{.title = "The table",
                                   .screen = "samples",
                                   .params = {{"page", ui::Value{2}}},
                                   .target = "samples_table"}}}};
    auto const written = author::withTours(R"({"vocab":"ui/1","id":"app"})", tours);
    REQUIRE(written.has_value());
    auto const loaded = ui::loadDocumentText(*written);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->tours.size() == 1);
    REQUIRE(loaded->tours.front().steps.size() == 3);
    CHECK(loaded->tours.front().steps.at(0).target == "shell_search");
    CHECK(loaded->tours.front().steps.at(1).advanceOn == "samples");
    CHECK(loaded->tours.front().steps.at(2).params.at("page") == ui::Value{2});
    // A document that declares tours already, and one that is not an object, are refused.
    CHECK_FALSE(author::withTours(*written, tours).has_value());
    CHECK_FALSE(author::withTours("[]", tours).has_value());
}

// Mutation: number the step from 0 in `author::tourJson`'s message, or skip the title checks (the missing title throws
// a generic operand error).
TEST_CASE("tours document: an absent title is refused naming the tour and the step", "[ui][tours][author]") {
    namespace author = ui::author;
    auto const refusalOf = [](std::vector<author::Tour> const& tours) {
        auto const written = author::withTours(R"({"vocab":"ui/1","id":"app"})", tours);
        REQUIRE_FALSE(written.has_value());
        return written.error().reason;
    };
    CHECK(refusalOf({author::Tour{.id = "lost", .steps = {author::TourStep{.title = "S"}}}}).contains("`lost`"));
    std::string const step = refusalOf(
        {author::Tour{.id = "gap", .title = "T", .steps = {author::TourStep{.title = "S"}, author::TourStep{}}}});
    CHECK(step.contains("`gap`"));
    // The second step is "step 2": steps are numbered from 1, like every other message.
    CHECK(step.contains("step 2 of"));
}

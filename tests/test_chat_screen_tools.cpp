// SPDX-License-Identifier: Apache-2.0
//
// An assistant's tools come from the screen it is on: what the document names, bound to what the screen holds.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <map>
#include <morph/ui/chat/screen_tools.hpp>
#include <morph/ui/document.hpp>
#include <regex>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace ui = morph::ui;
namespace chat = morph::ui::chat;

namespace {

ui::Document document(std::string_view text) {
    auto loaded = ui::loadDocumentText(text);
    REQUIRE(loaded.has_value());
    return *loaded;
}

constexpr std::string_view kScreen = R"({
  "vocab": "ui/1", "id": "lab.screen",
  "models": { "lab": { "type": "Lab" } },
  "queries": { "list": { "model": "lab", "action": "List" } },
  "mutations": { "add": { "model": "lab", "action": "Add" } },
  "view": { "kind": "text", "text": "Lab" } })";

constexpr std::string_view kTwoInstances = R"({
  "vocab": "ui/1", "id": "lab.two",
  "models": { "left": { "type": "Lab" }, "right": { "type": "Lab" } },
  "mutations": { "addLeft": { "model": "left", "action": "Add" }, "addRight": { "model": "right", "action": "Add" } },
  "view": { "kind": "text", "text": "Lab" } })";

std::vector<chat::ModelRef> refs(ui::Document const& loaded) {
    std::vector<chat::ModelRef> out;
    std::uint64_t next = 11;
    for (ui::ModelDecl const& model : loaded.scope.models) {
        out.push_back(
            {.path = model.path, .alias = model.name, .modelType = model.type, .instanceId = next, .primary = ""});
        next += 11;
    }
    return out;
}

constexpr std::string_view kNestedScopes = R"({
  "vocab": "ui/1", "id": "lab.nested",
  "models": { "lab": { "type": "Lab" } },
  "mutations": { "add": { "model": "lab", "action": "Add" } },
  "view": { "kind": "column", "children": [
    { "kind": "dialog", "id": "inner", "title": "Inner",
      "scope": { "models": { "lab": { "type": "Lab" } },
                 "mutations": { "addInner": { "model": "lab", "action": "Add" } } },
      "children": [ { "kind": "text", "text": "Inner" } ] } ] } })";

// The aliases of nested scopes, each with an id of its own.
// NOLINTNEXTLINE(misc-no-recursion): view nodes nest.
void collectNested(ui::DocNode const& node, std::vector<chat::ModelRef>& models) {
    if (node.scope) {
        for (ui::ModelDecl const& model : node.scope->models) {
            models.push_back({.path = model.path,
                              .alias = model.name,
                              .modelType = model.type,
                              .instanceId = 100 + models.size(),
                              .primary = ""});
        }
    }
    for (auto const& [key, children] : node.children) {
        static_cast<void>(key);
        for (ui::DocNodePtr const& child : children) {
            collectNested(*child, models);
        }
    }
}

std::vector<std::string> namesOf(std::vector<chat::BoundTool> const& tools) {
    std::vector<std::string> names;
    for (chat::BoundTool const& tool : tools) {
        names.push_back(tool.spec.name);
    }
    return names;
}

chat::BoundTool const& named(std::vector<chat::BoundTool> const& tools, std::string_view name) {
    auto const found =
        std::ranges::find_if(tools, [&](chat::BoundTool const& tool) { return tool.spec.name == name; });
    REQUIRE(found != tools.end());
    return *found;
}

bool allValid(std::vector<std::string> const& names) {
    static std::regex const pattern{"^[a-zA-Z0-9_-]{1,64}$"};
    return std::ranges::all_of(names, [](std::string const& name) { return std::regex_match(name, pattern); });
}

bool unique(std::vector<std::string> names) {
    std::ranges::sort(names);
    return std::ranges::adjacent_find(names) == names.end();
}

using Permitted = std::set<std::string, std::less<>>;
using Schemas = std::map<std::string, ui::Json, std::less<>>;

}  // namespace

TEST_CASE("a_screens_queries_and_mutations_become_tools", "[chat][tools]") {
    auto const loaded = document(kScreen);
    auto const models = refs(loaded);
    Schemas schemas;
    schemas.emplace("Add", ui::Json::object_t{{"properties", ui::Json::object_t{}}, {"description", "Adds a row."}});
    auto const tools = chat::screenTools(loaded, models, Permitted{"Add", "List"}, schemas);
    CHECK(namesOf(tools) == std::vector<std::string>{"lab__Add", "lab__List"});
    auto const& add = named(tools, "lab__Add");
    CHECK(add.action == "Add");
    CHECK(add.model.instanceId == 11);
    CHECK(add.spec.description.contains("Changes data"));
    CHECK(add.spec.description.contains("Adds a row."));
    CHECK(std::get<std::string>(add.spec.inputSchema["type"].data) == "object");
    CHECK(named(tools, "lab__List").spec.description.contains("Reads"));
    // A schema that did not arrive is still an object, which a model's tool format requires.
    CHECK(std::get<std::string>(named(tools, "lab__List").spec.inputSchema["type"].data) == "object");
}

TEST_CASE("an_unpermitted_action_gets_no_tool", "[chat][tools]") {
    auto const loaded = document(kScreen);
    auto const models = refs(loaded);
    CHECK(namesOf(chat::screenTools(loaded, models, Permitted{"List"}, {})) == std::vector<std::string>{"lab__List"});
}

TEST_CASE("a_model_the_context_did_not_bind_gets_no_tool", "[chat][tools]") {
    auto const loaded = document(kScreen);
    CHECK(chat::screenTools(loaded, {}, Permitted{"Add", "List"}, {}).empty());
}

TEST_CASE("a_context_naming_another_screens_alias_gets_no_tool", "[chat][tools]") {
    auto const loaded = document(kScreen);
    std::vector<chat::ModelRef> const models{
        {.path = "/models/other", .alias = "other", .modelType = "Lab", .instanceId = 5, .primary = ""}};
    CHECK(chat::screenTools(loaded, models, Permitted{"Add", "List"}, {}).empty());
}

TEST_CASE("a_context_naming_the_wrong_type_for_an_alias_gets_no_tool", "[chat][tools]") {
    auto const loaded = document(kScreen);
    auto models = refs(loaded);
    models.front().modelType = "Elsewhere";
    CHECK(chat::screenTools(loaded, models, Permitted{"Add", "List"}, {}).empty());
}

TEST_CASE("two_instances_of_one_type_are_told_apart_by_alias", "[chat][tools]") {
    auto const loaded = document(kTwoInstances);
    auto const models = refs(loaded);
    auto const tools = chat::screenTools(loaded, models, Permitted{"Add"}, {});
    REQUIRE(tools.size() == 2);
    auto const instanceOf = [&](std::string_view alias) {
        auto const found =
            std::ranges::find_if(models, [&](chat::ModelRef const& model) { return model.alias == alias; });
        REQUIRE(found != models.end());
        return found->instanceId;
    };
    CHECK(named(tools, "left__Add").model.instanceId == instanceOf("left"));
    CHECK(named(tools, "right__Add").model.instanceId == instanceOf("right"));
}

TEST_CASE("an_action_named_twice_on_one_model_gives_one_tool", "[chat][tools]") {
    auto const loaded = document(R"({
      "vocab": "ui/1", "id": "lab.twice",
      "models": { "lab": { "type": "Lab" } },
      "queries": { "one": { "model": "lab", "action": "List" }, "two": { "model": "lab", "action": "List" } },
      "mutations": { "again": { "model": "lab", "action": "List" } },
      "view": { "kind": "text", "text": "Lab" } })");
    auto const models = refs(loaded);
    auto const tools = chat::screenTools(loaded, models, Permitted{"List"}, {});
    REQUIRE(tools.size() == 1);
    // Named as a mutation anywhere, it is offered as changing data.
    CHECK(tools.front().spec.description.contains("Changes data"));
}

TEST_CASE("one_alias_in_two_scopes_gives_unique_names", "[chat][tools]") {
    auto const loaded = document(kNestedScopes);
    std::vector<chat::ModelRef> models = refs(loaded);
    collectNested(*loaded.view, models);
    REQUIRE(models.size() >= 2);
    auto const tools = chat::screenTools(loaded, models, Permitted{"Add"}, {});
    REQUIRE(tools.size() == 2);
    auto const names = namesOf(tools);
    CHECK(unique(names));
    CHECK(allValid(names));
    CHECK(tools.at(0).model.instanceId != tools.at(1).model.instanceId);
}

TEST_CASE("a_long_name_stays_valid_and_unique", "[chat][tools]") {
    std::string const stem(70, 'a');
    std::string const text = R"({ "vocab": "ui/1", "id": "lab.long", "models": { ")" + stem +
                             R"(": { "type": "Lab" } }, "mutations": { "one": { "model": ")" + stem +
                             R"(", "action": "Action.One" }, "two": { "model": ")" + stem +
                             R"(", "action": "Action.Two" } }, "view": { "kind": "text", "text": "Lab" } })";
    auto const loaded = document(text);
    auto const models = refs(loaded);
    auto const names = namesOf(chat::screenTools(loaded, models, Permitted{"Action.One", "Action.Two"}, {}));
    REQUIRE(names.size() == 2);
    CHECK(allValid(names));
    CHECK(unique(names));
}

TEST_CASE("a_credential_action_gets_no_tool", "[chat][tools]") {
    auto const loaded = document(kScreen);
    auto const models = refs(loaded);
    std::map<std::string, ui::ActionInfo, std::less<>> actions;
    actions.emplace("Add", ui::ActionInfo{.modelType = "Lab", .credential = true});
    CHECK(namesOf(chat::screenTools(loaded, models, Permitted{"Add", "List"}, {}, &actions)) ==
          std::vector<std::string>{"lab__List"});
}

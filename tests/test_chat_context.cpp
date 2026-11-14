// SPDX-License-Identifier: Apache-2.0
//
// What a mounted screen can say about itself for a chat turn: where the person is, and which instances it holds.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/ui/chat/protocol.hpp>
#include <morph/ui/host.hpp>
#include <morph/ui/queueing_dispatcher.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "ui_interpreter_support.hpp"

using morph::ui::Document;
using morph::ui::DocumentScreenHost;
using morph::ui::DocumentScreenHostOptions;
using morph::ui::ParamMap;
using morph::ui::Value;
using morph::ui::testing::Harness;
using morph::ui::testing::loadOrFail;

namespace {

constexpr std::string_view kScreen = R"({
  "vocab": "ui/1", "id": "sample", "params": { "sampleId": "int" },
  "identity": { "ref": "sampleId" },
  "models": { "lab": { "type": "Lims_SampleModel" } },
  "queries": { "read": { "model": "lab", "action": "Lims_Attach", "body": { "obj": {} } } },
  "view": { "kind": "column", "children": [
    { "kind": "dialog", "id": "inner", "title": "Inner",
      "scope": { "models": { "nested": { "type": "Lims_SampleModel" } },
                 "queries": { "innerRead": { "model": "nested", "action": "Lims_Attach", "body": { "obj": {} } } } },
      "children": [ { "kind": "text", "text": "Inner" } ] } ] } })";

constexpr std::string_view kKeyed = R"({
  "vocab": "ui/1", "id": "keyed", "params": { "sampleKey": "string" },
  "models": { "lab": { "type": "Lims_SampleModel", "instance": { "ref": "sampleKey" } } },
  "queries": { "read": { "model": "lab", "action": "Lims_Attach", "body": { "obj": {} } } },
  "view": { "kind": "text", "text": "Keyed" } })";

constexpr std::string_view kTitled = R"({
  "vocab": "ui/1", "id": "titled", "title": { "t": "titled.title", "fallback": "Sample 16/1" },
  "view": { "kind": "text", "text": "Titled" } })";

constexpr std::string_view kPlain = R"({
  "vocab": "ui/1", "id": "plain",
  "view": { "kind": "text", "text": "Plain" } })";

class Shell final : public DocumentScreenHost {
public:
    explicit Shell(Harness& harness) : DocumentScreenHost{harness.session, harness.dispatcher, options(harness)} {}

    using DocumentScreenHost::onNavigate;
    void onNavigate(std::string_view /*screenId*/, ParamMap const& /*params*/) override {}

private:
    static DocumentScreenHostOptions options(Harness& harness) {
        static std::map<std::string, std::shared_ptr<Document const>, std::less<>> const documents{
            {"sample", loadOrFail(kScreen, morph::ui::testing::corpusCatalog())},
            {"keyed", loadOrFail(kKeyed, morph::ui::testing::corpusCatalog())},
            {"plain", loadOrFail(kPlain, morph::ui::testing::corpusCatalog())},
            {"titled", loadOrFail(kTitled, morph::ui::testing::corpusCatalog())}};
        DocumentScreenHostOptions made;
        made.documents = [](std::string_view id) -> std::shared_ptr<Document const> {
            auto const found = documents.find(id);
            return found == documents.end() ? nullptr : found->second;
        };
        made.backend = &harness.backend;
        made.application = &harness.host;
        return made;
    }
};

}  // namespace

TEST_CASE("a_mounted_screen_reports_its_id_params_and_identity", "[chat][context]") {
    Harness harness;
    Shell shell{harness};
    auto mounted = shell.mount("sample", ParamMap{{"sampleId", Value{std::int64_t{42}}}});
    REQUIRE(mounted.has_value());
    harness.settle();
    auto const context = DocumentScreenHost::chatContext(**mounted);
    REQUIRE(context.has_value());
    CHECK(context->screenId == "sample");
    CHECK(context->paramsJson.contains("sampleId"));
    CHECK(context->paramsJson.contains("42"));
    CHECK_FALSE(context->identity.empty());
    CHECK(context->identity.contains("42"));
    CHECK(context->text.empty());
}

TEST_CASE("a_mounted_screen_reports_its_title", "[chat][context]") {
    Harness harness;
    Shell shell{harness};
    auto mounted = shell.mount("titled", ParamMap{});
    REQUIRE(mounted.has_value());
    harness.settle();
    auto const context = DocumentScreenHost::chatContext(**mounted);
    REQUIRE(context.has_value());
    CHECK(context->title == "Sample 16/1");
    CHECK(context->title == (*mounted)->title().peek());
}

TEST_CASE("a_screen_with_no_identity_reports_none", "[chat][context]") {
    Harness harness;
    Shell shell{harness};
    auto mounted = shell.mount("plain", ParamMap{});
    REQUIRE(mounted.has_value());
    harness.settle();
    auto const context = DocumentScreenHost::chatContext(**mounted);
    REQUIRE(context.has_value());
    CHECK(context->identity.empty());
    CHECK(context->models.empty());
}

TEST_CASE("a_private_alias_reports_the_id_its_channel_holds", "[chat][context]") {
    Harness harness;
    Shell shell{harness};
    auto mounted = shell.mount("sample", ParamMap{{"sampleId", Value{std::int64_t{42}}}});
    REQUIRE(mounted.has_value());
    harness.settle();
    auto const context = DocumentScreenHost::chatContext(**mounted);
    REQUIRE(context.has_value());
    REQUIRE(context->models.size() == 1);
    CHECK(context->models.front().alias == "lab");
    CHECK(context->models.front().modelType == "Lims_SampleModel");
    CHECK(context->models.front().instanceId == 1001);
    CHECK(context->models.front().primary.empty());
    // The path the server matches against the document's own declaration.
    auto const document = loadOrFail(kScreen, morph::ui::testing::corpusCatalog());
    CHECK(context->models.front().path == document->scope.models.front().path);
}

TEST_CASE("a_keyed_alias_reports_its_primary", "[chat][context]") {
    Harness harness;
    Shell shell{harness};
    auto mounted = shell.mount("keyed", ParamMap{{"sampleKey", Value{std::string{"S-42"}}}});
    REQUIRE(mounted.has_value());
    harness.settle();
    auto const context = DocumentScreenHost::chatContext(**mounted);
    REQUIRE(context.has_value());
    REQUIRE(context->models.size() == 1);
    CHECK(context->models.front().primary == "S-42");
}

TEST_CASE("an_alias_still_binding_is_left_out", "[chat][context]") {
    Harness harness;
    harness.dispatcher.factsBound = false;
    Shell shell{harness};
    auto mounted = shell.mount("sample", ParamMap{{"sampleId", Value{std::int64_t{42}}}});
    REQUIRE(mounted.has_value());
    harness.settle();
    auto const context = DocumentScreenHost::chatContext(**mounted);
    REQUIRE(context.has_value());
    CHECK(context->models.empty());
}

TEST_CASE("an_alias_of_a_nested_scope_is_left_out", "[chat][context]") {
    Harness harness;
    Shell shell{harness};
    auto mounted = shell.mount("sample", ParamMap{{"sampleId", Value{std::int64_t{42}}}});
    REQUIRE(mounted.has_value());
    harness.settle();
    auto const context = DocumentScreenHost::chatContext(**mounted);
    REQUIRE(context.has_value());
    for (morph::ui::chat::ModelRef const& model : context->models) {
        CHECK(model.alias != "nested");
    }
}

TEST_CASE("the_queueing_dispatcher_passes_facts_through", "[chat][context]") {
    Harness harness;
    morph::ui::QueueingDispatcher queueing{harness.dispatcher, &harness.owner};
    auto const channel = queueing.bind("Lims_SampleModel", morph::bridge::BindSharing::Shared, "S-1");
    CHECK(channel->facts().instanceId == 1001);
    CHECK(channel->facts().primary == "S-1");
}

TEST_CASE("a_bridge_channel_reports_the_instance_it_bound", "[chat][context]") {
    morph::testing::StepExecutor owner;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(owner), owner};
    morph::ui::BridgeDispatcher dispatcher{bridge, &owner};
    auto const channel = dispatcher.bind("Lims_SampleModel", morph::bridge::BindSharing::Private, "");
    REQUIRE(channel != nullptr);
    owner.runAll();
    CHECK(channel->facts().primary.empty());
}

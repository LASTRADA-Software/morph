// SPDX-License-Identifier: Apache-2.0

// The explorer's search over screens, models and actions: a model's type shows all its actions, an action's id shows
// that action alone and opens its model, and what matches nothing is hidden.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <functional>
#include <map>
#include <memory>
#include <morph/ui/bundle.hpp>
#include <morph/ui/explorer.hpp>
#include <morph/ui/host.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ui_interpreter_support.hpp"

using morph::ui::ActionInfo;
using morph::ui::Catalog;
using morph::ui::CatalogEntry;
using morph::ui::CatalogReply;
using morph::ui::Document;
using morph::ui::DocumentScreenHost;
using morph::ui::DocumentScreenHostOptions;
using morph::ui::Explorer;
using morph::ui::ExplorerOptions;
using morph::ui::ModelInfo;
using morph::ui::ParamMap;
using morph::ui::SchemasByModel;
using morph::ui::testing::corpusCatalog;
using morph::ui::testing::Harness;
using morph::ui::testing::loadOrFail;

namespace {

// A query and a mutation at the screen, a form and a run in the view, and a dialog whose own alias binds another
// model.
constexpr std::string_view kSamples = R"({
  "vocab": "ui/1", "id": "samples",
  "models": { "sample": { "type": "Lims_SampleModel" } },
  "queries": { "list": { "model": "sample", "action": "Lims_ListSamples" } },
  "mutations": { "remove": { "model": "sample", "action": "Lims_DeleteSample" } },
  "view": { "kind": "column", "children": [
    { "kind": "button", "label": "Remove", "onClick": [ { "run": "remove" } ] },
    { "kind": "form", "model": "sample", "action": "Lims_CaptureConcentration" },
    { "kind": "dialog", "id": "pick",
      "scope": { "models": { "clients": { "type": "Lims_ClientModel" } },
                 "queries": { "clientList": { "model": "clients", "action": "Lims_ClientList" } } },
      "children": [ { "kind": "text", "text": "pick" } ] } ] } })";

/// The schemas document of the sample model: two actions with schemas, one the catalog has no facts for.
std::string sampleSchemas() {
    Catalog const corpus = corpusCatalog();
    return R"({"Lims_CaptureConcentration":)" + corpus.schema("Lims_CaptureConcentration").value() +
           R"(,"Lims_ConfirmSample":)" + corpus.schema("Lims_ConfirmSample").value() +
           R"(,"Lims_Undescribed":{"type":"object","properties":{}}})";
}

/// A host over the documents the explorer provides.
class Host final : public DocumentScreenHost {
public:
    Host(Harness& harness, std::map<std::string, std::shared_ptr<Document const>, std::less<>>& documents)
        : DocumentScreenHost{harness.session, harness.dispatcher, options(harness, documents)} {}

    using DocumentScreenHost::onNavigate;
    void onNavigate(std::string_view screenId, ParamMap const& params) override {
        static_cast<void>(screenId);
        static_cast<void>(params);
    }

private:
    static DocumentScreenHostOptions options(
        Harness& harness, std::map<std::string, std::shared_ptr<Document const>, std::less<>>& documents) {
        DocumentScreenHostOptions made;
        made.documents = [&documents](std::string_view id) -> std::shared_ptr<Document const> {
            auto const found = documents.find(id);
            return found == documents.end() ? nullptr : found->second;
        };
        made.backend = &harness.backend;
        return made;
    }
};

/// The catalog of two models: the sample model with facts for one of its actions, and a model the server would not
/// describe.
CatalogReply twoModels() {
    CatalogReply reply;
    reply.entries = {CatalogEntry{.id = "samples", .title = "Samples", .module = "Lab"}};
    reply.models = std::map<std::string, ModelInfo, std::less<>>{{"Lims_SampleModel", ModelInfo{}},
                                                                 {"Lims_SecretModel", ModelInfo{}}};
    ActionInfo capture;
    capture.modelType = "Lims_SampleModel";
    capture.calculatedFields = {"concentration"};
    capture.effects = std::vector<morph::ui::ActionEffect>{
        morph::ui::ActionEffect{.kind = "changes", .record = "T_Row"},
        morph::ui::ActionEffect{.kind = "changesInstance", .record = std::nullopt}};
    capture.records = {morph::ui::ActionRecordInfo{.record = "T_Row", .keyField = "id", .paths = {"rows"}},
                       morph::ui::ActionRecordInfo{.record = "T_Note", .keyField = std::nullopt, .paths = {""}}};
    ActionInfo confirm;
    confirm.modelType = "Lims_SampleModel";
    reply.actions = std::map<std::string, ActionInfo, std::less<>>{{"Lims_CaptureConcentration", capture},
                                                                   {"Lims_ConfirmSample", confirm}};
    reply.permitted = {"Lims_CaptureConcentration"};
    return reply;
}

struct Fixture {
    Harness harness;
    std::map<std::string, std::shared_ptr<Document const>, std::less<>> provided;
    Host host{harness, provided};
    std::vector<std::string> opened;
    std::unique_ptr<morph::ui::StackWidget> page = harness.backend.createStack(nullptr, morph::ui::Axis::Vertical);

    std::unique_ptr<Explorer> explorer(std::function<void(ExplorerOptions&)> const& adjust = {}) {
        ExplorerOptions options;
        options.catalog = twoModels();
        options.facts = corpusCatalog();
        options.schemas = SchemasByModel{{"Lims_SampleModel", sampleSchemas()},
                                         {"Lims_SecretModel", std::unexpected(std::string{"not authorized"})}};
        options.documents = {{"samples", loadOrFail(kSamples, corpusCatalog())}};
        options.host = &host;
        options.provide = [this](std::string const& screenId, std::shared_ptr<Document const> document) {
            provided.insert_or_assign(screenId, std::move(document));
        };
        options.onOpen = [this](std::string const& screenId) { opened.push_back(screenId); };
        if (adjust) {
            adjust(options);
        }
        auto made = std::make_unique<Explorer>(harness.runtime, harness.backend, *page, std::move(options));
        harness.settle();
        return made;
    }

    [[nodiscard]] int byTestId(std::string_view kind, std::string_view testId) const {
        auto const found = harness.backend.find(kind, "testId", testId);
        if (!found) {
            FAIL("no " << kind << " with test id " << testId);
        }
        return *found;
    }

    [[nodiscard]] bool shows(std::string_view kind, std::string_view text) const {
        return harness.backend.find(kind, "text", text).has_value();
    }
};

}  // namespace

TEST_CASE("covj explorer: a search shows what it names and hides the rest", "[covj][ui][explorer]") {
    struct Case {
        std::string needle;
        bool screen;
        bool capture;
        bool undescribed;
        bool sampleModel;
        bool secretModel;
    };
    auto const [needle, screen, capture, undescribed, sampleModel, secretModel] =
        GENERATE(Case{"", true, true, true, true, true}, Case{"sample", true, true, true, true, false},
                 Case{"UNDESCRIBED", false, false, true, true, false},
                 Case{"secret", false, false, false, false, true}, Case{"zzz", false, false, false, false, false});
    CAPTURE(needle);
    Fixture f;
    auto const explorer = f.explorer();
    f.harness.backend.edit(f.byTestId("TextInput", "explorer_search"), needle);
    auto const visible = [&f](std::string_view kind, std::string_view testId) {
        return f.harness.backend.prop(f.byTestId(kind, testId), "visible") != "false";
    };
    CHECK(visible("Button", "explorer_screen_samples") == screen);
    CHECK(visible("Button", "explorer_action_Lims_CaptureConcentration") == capture);
    CHECK(visible("Button", "explorer_action_Lims_Undescribed") == undescribed);
    CHECK(visible("Collapsible", "explorer_model_Lims_SampleModel") == sampleModel);
    CHECK(visible("Collapsible", "explorer_model_Lims_SecretModel") == secretModel);
}

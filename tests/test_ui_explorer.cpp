// SPDX-License-Identifier: Apache-2.0
//
// The app explorer (docs/spec/ui/explorer.md): the places a document names an action, an action's fields as the forms
// engine reads them, the generated "Try it" document and the catalog it loads with, and the explorer screen itself.

#include <catch2/catch_test_macros.hpp>
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

using morph::ui::ActionField;
using morph::ui::ActionInfo;
using morph::ui::ActionUse;
using morph::ui::ActionUseKind;
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
using morph::ui::TryItKind;
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

    std::unique_ptr<Explorer> explorer() {
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

// Mutation: drop the scope's queries from the walk (the `list` use goes missing).
TEST_CASE("actionUses: queries, mutations and nodes at every scope, each with its pointer and model type",
          "[ui][explorer]") {
    auto const document = loadOrFail(kSamples, corpusCatalog());
    std::vector<ActionUse> const uses = morph::ui::actionUses(*document);
    REQUIRE(uses.size() == 4U);
    CHECK(uses.at(0).actionId == "Lims_ListSamples");
    CHECK(uses.at(0).kind == ActionUseKind::Query);
    CHECK(uses.at(0).pointer == "/queries/list");
    CHECK(uses.at(0).modelType == "Lims_SampleModel");
    CHECK(uses.at(1).actionId == "Lims_DeleteSample");
    CHECK(uses.at(1).kind == ActionUseKind::Mutation);
    CHECK(uses.at(1).pointer == "/mutations/remove");
    // The run names the mutation, not an action: the form comes next.
    CHECK(uses.at(2).actionId == "Lims_CaptureConcentration");
    CHECK(uses.at(2).kind == ActionUseKind::Form);
    CHECK(uses.at(2).pointer == "/view/children/1");
    // The dialog's own alias binds the client model.
    CHECK(uses.at(3).actionId == "Lims_ClientList");
    CHECK(uses.at(3).modelType == "Lims_ClientModel");
    CHECK(uses.at(3).pointer == "/view/children/2/scope/queries/clientList");
    CHECK(morph::ui::actionUseKindName(ActionUseKind::Wizard) == "wizard");
}

// Mutation: ignore `required` (the sample field reads optional).
TEST_CASE("actionFields: rows in x-order with the type, required and the bounds and rules", "[ui][explorer]") {
    auto const fields = morph::ui::actionFields(R"({"type":"object","properties":{
      "role":{"enum":["Viewer","Editor"],"x-order":1},
      "count":{"type":"integer","minimum":1,"maximum":10,"x-order":0},
      "note":{"type":["string","null"],"x-order":2}},
      "required":["count","role"],
      "x-rules":[{"kind":"requiredWhen","fields":["note"],"when":{"kind":"engaged","fields":["count"]}}]})");
    REQUIRE(fields.has_value());
    REQUIRE(fields->size() == 3U);
    ActionField const& count = fields->at(0);
    CHECK(count.name == "count");
    CHECK(count.type == "integer");
    CHECK(count.required);
    CHECK(count.rules == std::vector<std::string>{"min 1", "max 10"});
    CHECK(fields->at(1).name == "role");
    CHECK(fields->at(1).type == "one of: Viewer, Editor");
    CHECK(fields->at(2).name == "note");
    CHECK_FALSE(fields->at(2).required);
    CHECK(fields->at(2).rules == std::vector<std::string>{"requiredWhen"});

    auto const none = morph::ui::actionFields(R"({"type":"object","properties":{}})");
    REQUIRE(none.has_value());
    CHECK(none->empty());
    CHECK_FALSE(morph::ui::actionFields("not json").has_value());
}

// Mutation: drop the documents' actions from tryItCatalog (Lims_Undescribed is refused at load).
TEST_CASE("tryItCatalog answers an action only its model's schemas list; its document loads and mounts",
          "[ui][explorer]") {
    Catalog base = corpusCatalog();
    base.action = [](std::string_view action) -> std::optional<ActionInfo> {
        if (action == "Lims_CaptureConcentration") {
            ActionInfo info;
            info.modelType = "Lims_SampleModel";
            return info;
        }
        return std::nullopt;
    };
    Catalog const catalog = morph::ui::tryItCatalog(base, SchemasByModel{{"Lims_SampleModel", sampleSchemas()}});
    REQUIRE(catalog.action("Lims_Undescribed").has_value());
    CHECK(catalog.action("Lims_Undescribed")->modelType == "Lims_SampleModel");
    CHECK_FALSE(catalog.action("Lims_Nothing").has_value());
    CHECK(catalog.schema("Lims_Undescribed").value() == R"({"type":"object","properties":{}})");

    // Without the documents' actions the base refuses it.
    CHECK_FALSE(
        morph::ui::loadDocumentText(morph::ui::tryItDocument("Lims_Undescribed", "Lims_SampleModel", TryItKind::Form),
                                    morph::ui::LoadOptions{.catalog = base})
            .has_value());
    for (TryItKind const kind : {TryItKind::Form, TryItKind::Collection, TryItKind::Wizard}) {
        auto const loaded = morph::ui::loadDocumentText(
            morph::ui::tryItDocument("Lims_CaptureConcentration", "Lims_SampleModel", kind),
            morph::ui::LoadOptions{.catalog = catalog});
        INFO((loaded ? std::string{} : loaded.error().message()));
        CHECK(loaded.has_value());
    }
}

// Mutation: make the generated form submit automatically (one call before the button is pressed).
TEST_CASE("tryItDocument: the form mounts, submits from its button, and shows the reply and the failure",
          "[ui][explorer]") {
    Harness h;
    auto const screen =
        h.mount(morph::ui::tryItDocument("Lims_CaptureConcentration", "Lims_SampleModel", TryItKind::Form));
    CHECK(h.diagnostics.empty());
    h.backend.edit(h.byTestId("TextInput", "field_sampleId"), "4");
    h.backend.edit(h.byTestId("TextInput", "field_concentration"), "1");
    h.settle();
    CHECK(h.dispatcher.count("Lims_CaptureConcentration") == 0);
    h.backend.click(h.byTestId("Button", "form_submit"));
    h.settle();
    REQUIRE(h.dispatcher.count("Lims_CaptureConcentration") == 1);
    CHECK(h.dispatcher.calls.at(h.dispatcher.last("Lims_CaptureConcentration")).typeId == "Lims_SampleModel");
    h.reply(h.dispatcher.last("Lims_CaptureConcentration"), R"({"ok":true})");
    CHECK(h.text("tryit_result") == R"({"ok":true})");
    CHECK(h.text("tryit_error").empty());

    h.backend.click(h.byTestId("Button", "form_submit"));
    h.settle();
    h.fail(h.dispatcher.last("Lims_CaptureConcentration"));
    CHECK(h.text("tryit_error") == "Failed: it failed");
    CHECK(h.text("tryit_result").empty());
}

// Mutation: leave the refused model out of the list (its collapsible is missing).
TEST_CASE("Explorer: the screens and every model the catalog names, a refused one with its reason", "[ui][explorer]") {
    Fixture f;
    auto const explorer = f.explorer();
    CHECK(explorer->title().get() == "Explorer");
    CHECK_FALSE(explorer->dirty().get());
    CHECK(f.shows("Text", "Screens (1)"));
    CHECK(f.shows("Text", "Models (2)"));
    CHECK(f.harness.backend.find("Collapsible", "testId", "explorer_model_Lims_SampleModel").has_value());
    CHECK(f.harness.backend.find("Collapsible", "testId", "explorer_model_Lims_SecretModel").has_value());
    CHECK(f.shows("Text", "not described: not authorized"));
    // Every action of the model's schemas document, including one no screen names.
    CHECK(f.harness.backend.find("Button", "testId", "explorer_action_Lims_Undescribed").has_value());
    CHECK(f.harness.backend.find("Text", "testId", "explorer_hint").has_value());

    f.harness.backend.click(f.byTestId("Button", "explorer_screen_samples"));
    CHECK(f.opened == std::vector<std::string>{"samples"});

    // The search hides what it does not match.
    f.harness.backend.edit(f.byTestId("TextInput", "explorer_search"), "confirm");
    CHECK(f.harness.backend.prop(f.byTestId("Button", "explorer_action_Lims_CaptureConcentration"), "visible") ==
          "false");
    CHECK(f.harness.backend.prop(f.byTestId("Button", "explorer_action_Lims_ConfirmSample"), "visible") != "false");
    CHECK(f.harness.backend.prop(f.byTestId("Button", "explorer_screen_samples"), "visible") == "false");
}

// Mutation: skip showUses (the "Used by" card is missing).
TEST_CASE("Explorer: an action shows its facts, fields, uses, a live Try it and its schema", "[ui][explorer]") {
    Fixture f;
    auto const explorer = f.explorer();
    f.harness.backend.click(f.byTestId("Button", "explorer_action_Lims_CaptureConcentration"));
    f.harness.settle();
    CHECK(explorer->selected() == "Lims_CaptureConcentration");
    CHECK(f.harness.backend.prop(f.byTestId("Text", "explorer_title"), "text") ==
          "Lims_SampleModel / Lims_CaptureConcentration");
    for (std::string_view const section : {"explorer_facts", "explorer_fields", "explorer_uses", "explorer_tryit"}) {
        CHECK(f.harness.backend.find("Panel", "testId", section).has_value());
    }
    CHECK(f.harness.backend.find("Collapsible", "testId", "explorer_schema").has_value());
    CHECK(f.shows("Text", "server-calculated"));
    CHECK(f.shows("Text", "permitted"));
    CHECK(f.shows("Text", "quantity in mg/L"));
    CHECK(f.shows("Text", "samples  /view/children/1  (form)"));

    // "Try it" is the generated form, mounted live through the host.
    REQUIRE(f.provided.contains("explorer:try:Lims_CaptureConcentration"));
    f.harness.backend.edit(f.byTestId("TextInput", "field_sampleId"), "4");
    f.harness.backend.edit(f.byTestId("TextInput", "field_concentration"), "1");
    f.harness.settle();
    f.harness.backend.click(f.byTestId("Button", "form_submit"));
    f.harness.settle();
    CHECK(f.harness.dispatcher.count("Lims_CaptureConcentration") == 1);

    f.harness.backend.click(f.byTestId("Button", "explorer_open_0"));
    CHECK(f.opened == std::vector<std::string>{"samples"});

    // Choosing another action replaces the sections, and the "Try it" screen with them.
    REQUIRE(explorer->select("Lims_ConfirmSample"));
    f.harness.settle();
    CHECK_FALSE(f.harness.backend.find("TextInput", "testId", "field_concentration").has_value());
    CHECK(f.shows("Text", "not permitted"));
    CHECK(f.harness.backend.prop(f.byTestId("Text", "explorer_tryit_status"), "text") == "not permitted");
    CHECK(f.shows("Text", "No loaded document names it."));
    CHECK_FALSE(explorer->select("Lims_Nothing"));
}

// Mutation: treat every action without facts as not permitted (Try it is withheld).
TEST_CASE("Explorer: an action the catalog has no facts for is decided when it runs, and offered", "[ui][explorer]") {
    Fixture f;
    auto const explorer = f.explorer();
    REQUIRE(explorer->select("Lims_Undescribed"));
    f.harness.settle();
    CHECK(f.shows("Text", "decided when it runs"));
    CHECK(f.shows("Text", "No fields."));
    CHECK(f.provided.contains("explorer:try:Lims_Undescribed"));
    bool closed = false;
    explorer->requestClose([&closed](bool done) { closed = done; });
    CHECK(closed);
}

// Mutation: show `reads` for every action (the declared effects go missing), or drop the key field from a record.
TEST_CASE("facts_show_changes_record_and_records_with_key_and_path", "[ui][explorer][push]") {
    Fixture f;
    auto const explorer = f.explorer();
    REQUIRE(explorer->select("Lims_CaptureConcentration"));
    f.harness.settle();
    CHECK(f.shows("Text", "effects"));
    CHECK(f.shows("Text", "changes T_Row, changes its instance"));
    CHECK(f.shows("Text", "records"));
    CHECK(f.shows("Text", "T_Row by id at rows; T_Note keyless at the result"));
}

TEST_CASE("facts_show_reads_when_undeclared", "[ui][explorer][push]") {
    Fixture f;
    auto const explorer = f.explorer();
    REQUIRE(explorer->select("Lims_ConfirmSample"));
    f.harness.settle();
    CHECK(f.shows("Text", "effects"));
    CHECK(f.shows("Text", "reads"));
    CHECK_FALSE(f.shows("Text", "records"));
}

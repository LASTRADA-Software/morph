// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <morph/ui/document.hpp>
#include <string>

#include "ui_corpus.hpp"

using morph::ui::ActionInfo;
using morph::ui::CommandKind;
using morph::ui::Concurrency;
using morph::ui::Document;
using morph::ui::InstanceMode;
using morph::ui::Json;
using morph::ui::loadDocument;
using morph::ui::loadDocumentText;
using morph::ui::LoadOptions;
using morph::ui::ModelInfo;
using morph::ui::WatchSource;
using morph::ui::testing::member;
using morph::ui::testing::optionalMember;

namespace {

Json const& documentsCorpus() {
    static Json const corpus = morph::ui::testing::loadCorpus(MORPH_UI_DOCUMENTS_CORPUS);
    return corpus;
}

std::vector<std::string> stringsOf(Json const* json) {
    std::vector<std::string> out;
    if (json != nullptr) {
        for (Json const& entry : std::get<Json::array_t>(json->data)) {
            out.push_back(std::get<std::string>(entry.data));
        }
    }
    return out;
}

/// The corpus catalog, answered through the two lookups a client or a server supplies.
LoadOptions catalogOptions() {
    Json const& catalog = member(documentsCorpus(), "catalog");
    Json const& models = member(catalog, "models");
    Json const& actions = member(catalog, "actions");
    LoadOptions options;
    options.catalog.model = [&models](std::string_view type) -> std::optional<ModelInfo> {
        Json const* const entry = optionalMember(models, type);
        if (entry == nullptr) {
            return std::nullopt;
        }
        ModelInfo info;
        info.keyed = optionalMember(*entry, "keyed") != nullptr;
        info.stateless = optionalMember(*entry, "stateless") != nullptr;
        return info;
    };
    options.catalog.action = [&actions](std::string_view action) -> std::optional<ActionInfo> {
        Json const* const entry = optionalMember(actions, action);
        if (entry == nullptr) {
            return std::nullopt;
        }
        ActionInfo info;
        info.modelType = std::get<std::string>(member(*entry, "model").data);
        info.calculatedFields = stringsOf(optionalMember(*entry, "calculated"));
        info.secretFields = stringsOf(optionalMember(*entry, "secret"));
        info.credential = optionalMember(*entry, "credential") != nullptr;
        if (Json const* const lists = optionalMember(*entry, "lists")) {
            info.listFields = stringsOf(lists);
        }
        return info;
    };
    return options;
}

/// A case's document: given whole (`raw`), or the corpus base with the case's keys laid over it and `omit` removed.
Json caseDocument(Json const& entry) {
    if (Json const* const raw = optionalMember(entry, "raw")) {
        return *raw;
    }
    Json doc = member(documentsCorpus(), "base");
    auto& fields = std::get<Json::object_t>(doc.data);
    if (Json const* const overlay = optionalMember(entry, "doc")) {
        for (auto const& [key, value] : std::get<Json::object_t>(overlay->data)) {
            fields[key] = value;
        }
    }
    for (std::string const& key : stringsOf(optionalMember(entry, "omit"))) {
        fields.erase(key);
    }
    return doc;
}

}  // namespace

// Mutation: drop the row-scope `instance` rule -> "a row model alias without an instance" is accepted.
// Mutation: skip the let cycle check (derivedType returns `any` while visiting) -> "a let cycle" is accepted.
TEST_CASE("ui document: the invalid corpus is refused with its path and reason", "[ui][document]") {
    LoadOptions const options = catalogOptions();
    std::size_t refused = 0;
    for (Json const& entry : std::get<Json::array_t>(member(documentsCorpus(), "invalid").data)) {
        std::string const name = std::get<std::string>(member(entry, "name").data);
        CAPTURE(name);
        Json const doc = caseDocument(entry);
        bool const needsCatalog = optionalMember(entry, "catalog") != nullptr;
        auto const loaded = needsCatalog ? loadDocument(doc, options) : loadDocument(doc);
        REQUIRE_FALSE(loaded.has_value());
        CAPTURE(loaded.error().message());
        CHECK(loaded.error().path == std::get<std::string>(member(entry, "at").data));
        CHECK(loaded.error().reason.contains(std::get<std::string>(member(entry, "reason").data)));
        if (needsCatalog) {
            // The catalog rules are the only ones these documents break.
            auto const withoutCatalog = loadDocument(doc);
            if (!withoutCatalog) {
                FAIL("refused without a catalog: " << withoutCatalog.error().message());
            }
        }
        ++refused;
    }
    CHECK(refused > 200);
}

// Mutation: make loadDocument refuse every `custom` node -> the reference screen is refused.
TEST_CASE("ui document: the valid corpus loads, with and without the catalog", "[ui][document]") {
    LoadOptions const options = catalogOptions();
    for (Json const& entry : std::get<Json::array_t>(member(documentsCorpus(), "valid").data)) {
        std::string const name = std::get<std::string>(member(entry, "name").data);
        CAPTURE(name);
        Json const& doc = member(entry, "doc");
        bool const catalogToo = optionalMember(entry, "noCatalog") == nullptr;
        for (bool const withCatalog : {false, catalogToo}) {
            auto const loaded = withCatalog ? loadDocument(doc, options) : loadDocument(doc);
            if (!loaded) {
                FAIL("refused: " << loaded.error().message());
            }
        }
    }
}

// Mutation: store the params in a map rather than in declaration order -> the params order check fails.
TEST_CASE("ui document: the model holds what the document declares", "[ui][document]") {
    Json const& entry = std::get<Json::array_t>(member(documentsCorpus(), "valid").data).front();
    auto const loaded = loadDocument(member(entry, "doc"), catalogOptions());
    REQUIRE(loaded.has_value());
    Document const& doc = *loaded;
    CHECK(doc.vocab == "ui/1");
    CHECK(doc.id == "lims.results");
    REQUIRE(doc.params.size() == 2);
    CHECK(doc.params.at(0).first == "sampleId");
    CHECK(doc.params.at(0).second.name() == "int");
    CHECK(doc.params.at(1).second.name() == "string?");
    CHECK(doc.title != nullptr);
    CHECK(doc.identity != nullptr);
    CHECK(doc.dirty != nullptr);
    CHECK(doc.showErrors == "touched");
    CHECK(doc.lifecycle.size() == 4);
    CHECK(doc.keys.size() == 9);
    CHECK(doc.prefetch == std::vector<std::string>{"results", "prefetchable"});

    REQUIRE(doc.scope.models.size() == 6);
    CHECK(doc.scope.models.at(5).instance == InstanceMode::Keyed);
    CHECK(doc.scope.models.at(0).instance == InstanceMode::Keyed);
    CHECK(doc.scope.models.at(0).key != nullptr);
    CHECK(doc.scope.models.at(1).instance == InstanceMode::Default);
    CHECK(doc.scope.models.at(2).instance == InstanceMode::Shared);
    CHECK(doc.scope.models.at(3).instance == InstanceMode::Private);
    CHECK(doc.scope.models.at(4).instance == InstanceMode::Borrowed);
    CHECK(doc.scope.models.at(4).borrowedFrom == "sample");

    REQUIRE(doc.scope.queries.size() == 4);
    auto const& results = doc.scope.queries.at(0);
    CHECK(results.name == "results");
    CHECK(results.staleTime == 1000);
    CHECK_FALSE(results.evaluate);
    CHECK(doc.scope.queries.at(2).evaluate);

    REQUIRE(doc.scope.mutations.size() == 4);
    auto const& capture = doc.scope.mutations.at(0);
    CHECK(capture.concurrency == Concurrency::Serial);
    CHECK(capture.serialKey != nullptr);
    CHECK(capture.offlineQueue);
    CHECK(capture.optimistic.has_value());
    CHECK(capture.onSuccess.size() == 1);
    CHECK(capture.onSuccess.front().kind == CommandKind::Set);
    CHECK(doc.scope.mutations.at(1).concurrency == Concurrency::Exclusive);
    CHECK(doc.scope.mutations.at(2).concurrency == Concurrency::Latest);
    CHECK(doc.scope.mutations.at(3).concurrency == Concurrency::Parallel);

    REQUIRE(doc.scope.watches.size() == 3);
    CHECK(doc.scope.watches.at(0).source == WatchSource::User);
    CHECK(doc.scope.watches.at(1).source == WatchSource::Any);
    REQUIRE(doc.scope.checks.size() == 1);
    CHECK(doc.scope.checks.front().blocking);
    CHECK(doc.scope.checks.front().targets == std::vector<std::string>{"dilution"});

    REQUIRE(doc.components.size() == 2);
    auto const& chart = doc.components.at("ConcentrationChart");
    CHECK(chart.props.at("limit") == "exact");
    CHECK(chart.events.at("pointSelected").name() == "key");
    CHECK(chart.fallback != nullptr);
    CHECK(doc.components.at("ResultGrid").version == 2);
    CHECK(doc.customize.at("density").options.size() == 2);
    CHECK(doc.customize.at("density").defaultValue == morph::ui::Value{"comfortable"});
    CHECK(doc.customize.at("resultFilter").type.name() == "string?");

    REQUIRE(doc.view != nullptr);
    CHECK(doc.view->kind == "column");
    CHECK(doc.view->prop("gap") != nullptr);
    CHECK(doc.view->prop("nope") == nullptr);
    CHECK(doc.view->event("nope") == nullptr);
    CHECK(doc.view->nodes("nope").empty());
    CHECK(doc.view->partList("nope").empty());
    CHECK(doc.view->name("nope").empty());
    auto const children = doc.view->nodes("children");
    REQUIRE(children.size() > 10);
    auto const& heading = *children.front();
    CHECK(heading.id == "heading");
    CHECK(heading.name("testId") == "heading");
    REQUIRE(heading.partList("a11y").size() == 1);
    CHECK(heading.partList("a11y").front()->name("role") == "heading");
    auto const& button = *children[4];
    REQUIRE(button.event("onClick") != nullptr);
    CHECK(button.event("onClick")->front().kind == CommandKind::Seq);
    CHECK(button.event("onClick")->front().commands.size() == 3);
}

// Mutation: leave the document id out of the refusal -> the message check fails.
TEST_CASE("ui document: a refusal names the document", "[ui][document]") {
    auto const refused = loadDocumentText(R"({"vocab": "ui/1", "id": "lims.results", "view": {"kind": "nope"}})");
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().document == "lims.results");
    CHECK(refused.error().message() == "document lims.results at /view/kind: the node kind `nope` is not in ui/1");

    auto const notJson = loadDocumentText("{");
    REQUIRE_FALSE(notJson.has_value());
    CHECK(notJson.error().reason.starts_with("not JSON"));

    auto const noId = loadDocumentText(R"({"vocab": "ui/1", "id": 5})");
    REQUIRE_FALSE(noId.has_value());
    CHECK(noId.error().document.empty());
    CHECK(noId.error().path == "/id");

    auto const loaded = loadDocumentText(R"({"vocab": "ui/1", "id": "t", "view": {"kind": "text", "text": "x"}})");
    REQUIRE(loaded.has_value());
    CHECK(loaded->view->kind == "text");

    auto const kinds = morph::ui::nodeKinds();
    CHECK(kinds.size() == 39);
    CHECK(std::ranges::find(kinds, "chart") != kinds.end());
    CHECK(std::ranges::find(kinds, "dateTimeInput") != kinds.end());
    CHECK(std::ranges::find(kinds, "menuItem") == kinds.end());
}

TEST_CASE("ui document: a button's emphasis is normal, primary, danger or ghost", "[ui][document]") {
    CHECK(loadDocumentText(R"({"vocab": "ui/1", "id": "e", "view": {"kind": "column", "children": [
        {"kind": "button", "label": "a", "emphasis": "primary"},
        {"kind": "button", "label": "b", "emphasis": "danger"},
        {"kind": "button", "label": "c", "emphasis": "normal"},
        {"kind": "button", "label": "d", "emphasis": "ghost"}]}})")
              .has_value());
    auto const refused = loadDocumentText(
        R"({"vocab": "ui/1", "id": "e", "view": {"kind": "button", "label": "a", "emphasis": "loud"}})");
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().message() == "document e at /view/emphasis: `loud` is not one of the values of `emphasis`");
}

TEST_CASE("ui document: a dialog or drawer's literal sizing accepts fixed but refuses stretch", "[ui][document]") {
    CHECK(loadDocumentText(
              R"({"vocab": "ui/1", "id": "e", "view": {"kind": "dialog", "sizing": {"lit": {"fixed": 80}}}})")
              .has_value());
    auto const stretchingWidth = loadDocumentText(
        R"({"vocab": "ui/1", "id": "e", "view": {"kind": "dialog", "sizing": {"lit": {"stretch": 1}}}})");
    REQUIRE_FALSE(stretchingWidth.has_value());
    CHECK(stretchingWidth.error().message().contains("an overlay is sized by the frontend"));
    auto const stretchingHeight = loadDocumentText(
        R"({"vocab": "ui/1", "id": "e", "view": {"kind": "drawer",
            "sizing": {"lit": {"width": "content", "height": {"stretch": 1}}}}})");
    REQUIRE_FALSE(stretchingHeight.has_value());
    CHECK(stretchingHeight.error().message().contains("an overlay is sized by the frontend"));
}

// A date input shows a timestamp or an ISO-8601 date or date-time string; any other type it can never show.
// Mutation: drop the refusal in checkDateValue (the int case loads).
TEST_CASE("ui document: a date input's value is a timestamp or a string, and any other type is refused",
          "[ui][document]") {
    auto const refused = loadDocumentText(R"({"vocab": "ui/1", "id": "d",
        "state": { "n": { "init": 1 } },
        "view": { "kind": "dateTimeInput", "value": { "ref": "n" } }})");
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().path == "/view/value");
    CHECK(refused.error().reason ==
          "`dateTimeInput` shows a timestamp or an ISO-8601 date or date-time string; `value` is int");

    CHECK(loadDocumentText(R"({"vocab": "ui/1", "id": "d",
        "state": { "when": { "type": "string" } },
        "view": { "kind": "dateTimeInput", "value": { "ref": "when" } }})")
              .has_value());
    CHECK(loadDocumentText(R"({"vocab": "ui/1", "id": "d",
        "state": { "when": { "type": "timestamp" } },
        "view": { "kind": "dateTimeInput", "value": { "ref": "when" } }})")
              .has_value());
    CHECK(loadDocumentText(R"({"vocab": "ui/1", "id": "d",
        "state": { "when": { "type": "any" } },
        "view": { "kind": "dateTimeInput", "value": { "ref": "when" } }})")
              .has_value());
}

// Mutation: drop `spans` from the text's keys, and the first two documents are refused; drop the run check, and the
// third loads.
TEST_CASE("ui document: a text's spans load beside or in place of its text, and a run without text is refused",
          "[ui][document]") {
    CHECK(loadDocumentText(R"({"vocab": "ui/1", "id": "s", "view": {"kind": "text",
        "spans": {"list": ["Lot ", {"obj": {"text": "L-26-0381", "mono": true}}]}}})")
              .has_value());
    CHECK(loadDocumentText(R"({"vocab": "ui/1", "id": "s", "view": {"kind": "banner", "text": "Lot L-26-0381",
        "spans": {"list": ["Lot ", {"obj": {"text": "L-26-0381", "mono": true, "strong": false}}]}}})")
              .has_value());
    auto const noText = loadDocumentText(R"({"vocab": "ui/1", "id": "s", "view": {"kind": "text",
        "spans": {"list": ["Lot ", {"obj": {"strong": true}}]}}})");
    REQUIRE_FALSE(noText.has_value());
    CHECK(noText.error().path == "/view/spans/list/1");
    CHECK(noText.error().reason == "a run's record gives its `text`");
    auto const notAFlag = loadDocumentText(R"({"vocab": "ui/1", "id": "s", "view": {"kind": "text",
        "spans": {"list": [{"obj": {"text": "x", "mono": "yes"}}]}}})");
    REQUIRE_FALSE(notAFlag.has_value());
    CHECK(notAFlag.error().reason == "a run's `mono` is true or false");
    auto const number = loadDocumentText(R"({"vocab": "ui/1", "id": "s", "view": {"kind": "text",
        "spans": {"list": [7]}}})");
    REQUIRE_FALSE(number.has_value());
    CHECK(number.error().reason == "a run is a string or a record with `text`");
}

// Mutation: drop the keyValue's fieldLayout check (the grid layout loads).
TEST_CASE("ui document: a key-value's fieldLayout is inline or stacked, and its items' spans are checked",
          "[ui][document]") {
    CHECK(loadDocumentText(R"({"vocab": "ui/1", "id": "k", "view": {"kind": "keyValue", "fieldLayout": "stacked",
        "items": {"list": [{"obj": {"label": "Serial", "spans": {"list": [{"obj": {"text": "D-9", "mono": true}}]}}}]}}})")
              .has_value());
    auto const grid = loadDocumentText(R"({"vocab": "ui/1", "id": "k", "view": {"kind": "keyValue",
        "fieldLayout": "grid", "items": {"list": []}}})");
    REQUIRE_FALSE(grid.has_value());
    CHECK(grid.error().path == "/view/fieldLayout");
    auto const run = loadDocumentText(R"({"vocab": "ui/1", "id": "k", "view": {"kind": "keyValue",
        "items": {"list": [{"obj": {"label": "Serial", "spans": {"list": [{"obj": {"mono": true}}]}}}]}}})");
    REQUIRE_FALSE(run.has_value());
    CHECK(run.error().reason == "a run's record gives its `text`");
}

// Mutation: drop the text's refusal of `description` (the text loads).
TEST_CASE("ui document: an input takes a description and a tone, and a text refuses them", "[ui][document]") {
    CHECK(loadDocumentText(R"({"vocab": "ui/1", "id": "d", "view": {"kind": "textInput",
        "description": "Required range 2–8 °C", "tone": {"if": [true, "err", null]}}})")
              .has_value());
    auto const text = loadDocumentText(R"({"vocab": "ui/1", "id": "d", "view": {"kind": "text", "text": "x",
        "description": "y"}})");
    REQUIRE_FALSE(text.has_value());
    CHECK(text.error().path == "/view/description");
    CHECK(text.error().reason == "a text is not described; put the line in the text");
}

// Mutation: drop `group` from the kinds (every document here is refused for its kind).
TEST_CASE("ui document: a group loads with its legend and children, and its keys are checked as a column's",
          "[ui][document]") {
    CHECK(loadDocumentText(R"({"vocab": "ui/1", "id": "g", "view": {"kind": "group", "id": "conditions",
        "label": "Condition on receipt", "required": true, "gap": 1, "description": "Container, seal and label.",
        "children": [{"kind": "checkbox", "label": "Container intact"}]}})")
              .has_value());
    auto const gap = loadDocumentText(R"({"vocab": "ui/1", "id": "g", "view": {"kind": "group", "label": "x",
        "gap": {"bogus": 1}, "children": []}})");
    REQUIRE_FALSE(gap.has_value());
    CHECK(gap.error().path.starts_with("/view/gap"));
    auto const column = loadDocumentText(R"({"vocab": "ui/1", "id": "g", "view": {"kind": "column",
        "gap": {"bogus": 1}, "children": []}})");
    REQUIRE_FALSE(column.has_value());
    CHECK(column.error().reason == gap.error().reason);
}

// Mutation: drop checkMultilineExtras (both documents load, and the icon's mount throws).
TEST_CASE("ui document: a multiline text input refuses a unit and an icon", "[ui][document]") {
    CHECK(loadDocumentText(R"({"vocab": "ui/1", "id": "u", "view": {"kind": "textInput", "label": "T", "unit": "°C"}})")
              .has_value());
    auto const unit = loadDocumentText(R"({"vocab": "ui/1", "id": "u", "view": {"kind": "textInput", "label": "T",
        "mode": "multiline", "unit": "°C"}})");
    REQUIRE_FALSE(unit.has_value());
    CHECK(unit.error().path == "/view/unit");
    CHECK(unit.error().reason == "a multiline input shows no unit");
    auto const icon = loadDocumentText(R"({"vocab": "ui/1", "id": "u", "view": {"kind": "textInput", "label": "T",
        "mode": "multiline", "icon": "search"}})");
    REQUIRE_FALSE(icon.has_value());
    CHECK(icon.error().path == "/view/icon");
}

// Mutation: leave `dangerOutline` out of a button's emphases (the first document is refused).
TEST_CASE("ui document: dangerOutline, an outline badge and a segmented select load; other names are refused",
          "[ui][document]") {
    CHECK(loadDocumentText(R"({"vocab": "ui/1", "id": "c", "view": {"kind": "column", "children": [
        {"kind": "button", "label": "Reject", "emphasis": "dangerOutline"},
        {"kind": "badge", "text": "STAT", "tone": "err", "style": "outline"},
        {"kind": "select", "label": "Show", "style": "segmented", "options": {"list": []}}]}})")
              .has_value());
    auto const emphasis = loadDocumentText(R"({"vocab": "ui/1", "id": "c", "view": {"kind": "button", "label": "x",
        "emphasis": "outline"}})");
    REQUIRE_FALSE(emphasis.has_value());
    CHECK(emphasis.error().path == "/view/emphasis");
    auto const badge = loadDocumentText(R"({"vocab": "ui/1", "id": "c", "view": {"kind": "badge", "text": "x",
        "style": "ghost"}})");
    REQUIRE_FALSE(badge.has_value());
    CHECK(badge.error().path == "/view/style");
    auto const select = loadDocumentText(R"({"vocab": "ui/1", "id": "c", "view": {"kind": "select", "label": "x",
        "style": "radios"}})");
    REQUIRE_FALSE(select.has_value());
    CHECK(select.error().path == "/view/style");
    CHECK(select.error().reason == "`radios` is not one of the values of `style`");
}

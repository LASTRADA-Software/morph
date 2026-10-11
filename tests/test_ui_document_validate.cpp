// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <expected>
#include <fstream>
#include <morph/ui/document.hpp>
#include <morph/ui/expression.hpp>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace ui = morph::ui;

namespace {

struct CorpusDocument {
    std::string name;
    ui::Json document;
    std::vector<std::pair<std::string, std::string>> refusals;
};

struct Corpus {
    ui::Catalog catalog;
    std::vector<CorpusDocument> documents;
};

[[nodiscard]] ui::Type typeOf(ui::Json const& json, std::string_view key) {
    if (!json.contains(key)) {
        return {};
    }
    std::expected<ui::Type, std::string> type = ui::parseType(json[key].get_string());
    REQUIRE(type.has_value());
    return *type;
}

// Reads tests/data/ui/documents.json. Fails rather than returning an empty corpus: zero rows would pass every loop.
[[nodiscard]] Corpus const& corpus() {
    static Corpus const kCorpus = [] {
        std::ifstream file{MORPH_UI_DOCUMENT_CORPUS};
        REQUIRE(file.is_open());
        std::ostringstream text;
        text << file.rdbuf();
        ui::Json dom{};
        REQUIRE_FALSE(glz::read_json(dom, text.str()));
        Corpus out;
        ui::Json const& catalog = dom["catalog"];
        for (ui::Json const& model : catalog["models"].get_array()) {
            out.catalog.models.push_back(
                {.type = model["type"].get_string(),
                 .keyed = model.contains("keyed") && model["keyed"].get_boolean(),
                 .stateless = model.contains("stateless") && model["stateless"].get_boolean()});
        }
        for (ui::Json const& action : catalog["actions"].get_array()) {
            ui::Catalog::Action entry{
                .id = action["id"].get_string(),
                .model = action["model"].get_string(),
                .request = typeOf(action, "request"),
                .reply = typeOf(action, "reply"),
                .calculated = {},
                .credential = action.contains("credential") && action["credential"].get_boolean()};
            if (action.contains("calculated")) {
                for (ui::Json const& field : action["calculated"].get_array()) {
                    entry.calculated.push_back(field.get_string());
                }
            }
            out.catalog.actions.push_back(std::move(entry));
        }
        for (ui::Json const& component : catalog["components"].get_array()) {
            out.catalog.components.push_back(component.get_string());
        }
        for (ui::Json const& entry : dom["documents"].get_array()) {
            CorpusDocument document{.name = entry["name"].get_string(), .document = entry["document"], .refusals = {}};
            for (ui::Json const& refusal : entry["refusals"].get_array()) {
                document.refusals.emplace_back(refusal["path"].get_string(), refusal["reason"].get_string());
            }
            out.documents.push_back(std::move(document));
        }
        REQUIRE(out.documents.size() > 2);
        return out;
    }();
    return kCorpus;
}

// The refusals of a corpus document: its parse problem, or its validation's refusals.
[[nodiscard]] std::vector<std::pair<std::string, std::string>> refusalsOf(ui::Json const& json,
                                                                          ui::Catalog const& catalog) {
    std::expected<ui::Document, ui::Problem> document = ui::parseDocument(json);
    if (!document) {
        return {{document.error().path, document.error().reason}};
    }
    std::vector<std::pair<std::string, std::string>> out;
    for (ui::Refusal const& refusal : ui::validate(*document, catalog)) {
        out.emplace_back(refusal.path, refusal.reason);
    }
    return out;
}

}  // namespace

// Each corpus document is refused with exactly the paths and reasons it lists, and the valid ones with none.
// Mutations: resolve names outermost scope first (the shadowing document then refuses); skip the watch-cycle
// search; skip the row-scope instance rule; drop the bool check on node properties.
TEST_CASE("ui::validate: the document-validation corpus", "[ui][document]") {
    Corpus const& data = corpus();
    for (CorpusDocument const& entry : data.documents) {
        INFO(entry.name);
        CHECK(refusalsOf(entry.document, data.catalog) == entry.refusals);
    }
}

// Mutation: leave the document id off a refusal.
TEST_CASE("ui::validate: a refusal names its document", "[ui][document]") {
    ui::Json json{};
    REQUIRE_FALSE(glz::read_json(
        json, R"({"vocab": "ui/1", "id": "lims.home", "view": {"kind": "text", "text": {"ref": "x"}}})"));
    std::expected<ui::Document, ui::Problem> const document = ui::parseDocument(json);
    REQUIRE(document.has_value());
    CHECK(ui::validate(*document, ui::Catalog{}) ==
          std::vector<ui::Refusal>{
              {.document = "lims.home", .path = "/view/text", .reason = "`x` is not declared in scope"}});
}

// Mutations: read the reply type from the request; skip `x-computed` when collecting calculated fields.
TEST_CASE("ui::Catalog::addAction: the served schema gives the request, the reply and the calculated fields",
          "[ui][document]") {
    ui::Json schema{};
    REQUIRE_FALSE(
        glz::read_json(schema, R"({"type": "object", "required": ["id"], "properties": {"id": {"type": "integer"}},
        "x-reply": {"type": "array", "items": {"$ref": "#/$defs/Row"}, "$defs": {"Row": {"type": "object",
            "required": ["id"], "properties": {"id": {"type": "integer"},
            "concentration": {"type": "object", "x-computed": {"inputs": ["mass"]},
                              "properties": {"num": {"type": "integer"}, "den": {"type": "integer"}, "dp": {"type": "integer"}}},
            "label": {"type": "string", "x-readonly": true}}}}}})"));
    ui::Catalog catalog;
    catalog.addAction("Lims_ListResults", "Lims_SampleModel", schema);
    ui::Catalog::Action const* const action = catalog.action("Lims_ListResults");
    REQUIRE(action != nullptr);
    CHECK(action->request.text() == "{id: int}");
    CHECK(action->reply.text() == "list<{id: int, concentration: decimal?, label: string?}>");
    CHECK(action->calculated == std::vector<std::string>{"concentration", "label"});
    CHECK_FALSE(action->credential);
}

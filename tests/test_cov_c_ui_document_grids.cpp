// SPDX-License-Identifier: Apache-2.0
//
// Documents whose tables sit over a form: a fixed grid's rows and cells, a form grid's field, and the rules a
// table of a model or a form answers to. Each variant is refused with the path and the reason it breaks.

#include <catch2/catch_test_macros.hpp>
#include <map>
#include <morph/ui/document.hpp>
#include <string>
#include <string_view>
#include <vector>

#include "grid_test_support.hpp"

using morph::ui::ActionInfo;
using morph::ui::loadDocumentText;
using morph::ui::LoadOptions;
using morph::ui::ModelInfo;

namespace {

LoadOptions gridOptions(bool withSchema) {
    LoadOptions options;
    options.catalog.model = [](std::string_view type) -> std::optional<ModelInfo> {
        if (type == "Lims_SampleModel") {
            return ModelInfo{};
        }
        return std::nullopt;
    };
    options.catalog.action = [](std::string_view action) -> std::optional<ActionInfo> {
        if (action == "Lims_SetLimits") {
            return ActionInfo{.modelType = "Lims_SampleModel"};
        }
        return std::nullopt;
    };
    if (withSchema) {
        options.catalog.schema = [](std::string_view action) -> std::optional<std::string> {
            if (action == "Lims_SetLimits") {
                return std::string{morph::test::grid::kSetLimits};
            }
            return std::nullopt;
        };
    }
    return options;
}

std::string documentOf(std::string_view table) {
    return R"({"vocab":"ui/1","id":"d","models":{"sample":{"type":"Lims_SampleModel"}},"view":{"kind":"column",)"
           R"("children":[{"kind":"form","id":"rec","model":"sample","action":"Lims_SetLimits","submit":"explicit"},)" +
           std::string{table} + "]}}";
}

constexpr std::string_view kColumns = R"("columns":[{"id":"min","label":"Min"},{"id":"max","label":"Max"}])";

std::string fixed(
    std::string_view extra,
    std::string_view rows = R"("layoutRows":[{"key":"c","label":"C","cells":{"min":{"field":"limits.count"}}}])") {
    return R"({"kind":"table","id":"g","form":"rec",)" + std::string{kColumns} + "," + std::string{rows} +
           (extra.empty() ? "" : "," + std::string{extra}) + "}";
}

struct Case {
    std::string name;
    std::string table;
    std::string path;
    std::string reason;
    bool schema = true;
};

}  // namespace

TEST_CASE("a fixed grid that breaks a rule of its rows is refused with its path", "[covc][ui][document][grid]") {
    std::vector<Case> const cases{
        {"rows and layoutRows", fixed(R"("rows":{"lit":[]})"), "/view/children/1/layoutRows", "in place of `rows`"},
        {"a field of its own", fixed(R"("field":"specimens")"), "/view/children/1/field", "takes no `field`"},
        {"a sort", fixed(R"("sort":{"lit":"x"})"), "/view/children/1/sort", "it takes no `sort`"},
        {"row spans", fixed(R"("rowSpans":{"lit":1})"), "/view/children/1/rowSpans", "it takes no `rowSpans`"},
        {"appended rows", fixed(R"("appendRows":true)"), "/view/children/1/appendRows", "it takes no `appendRows`"},
        {"a key", fixed(R"("key":"id")"), "/view/children/1/key", "it takes no `key`"},
        {"column given as an expression",
         R"({"kind":"table","id":"g","form":"rec","columns":{"lit":1},"layoutRows":[{"key":"c","label":"C","cells":{}}]})",
         "/view/children/1/columns", "a list of column parts"},
        {"a column naming a field",
         R"({"kind":"table","id":"g","form":"rec","columns":[{"id":"min","label":"Min","field":"x"}],"layoutRows":[{"key":"c","label":"C","cells":{}}]})",
         "/view/children/1/columns/0", "name neither"},
        {"a form that is not one", fixed(R"("form":"nope")"), "", "is not a form of this document"},
        {"a block without a form",
         R"({"kind":"table","id":"g",)" + std::string{kColumns} +
             R"(,"layoutRows":[{"forEach":"specimens","rows":[{"key":"m","label":"M","cells":{"min":{"text":"x"}}}]}]})",
         "/view/children/1/layoutRows/0/forEach", "give the table its `form`"},
        {"a block with a key", fixed("", R"("layoutRows":[{"forEach":"specimens","key":"k","rows":[]}])"),
         "/view/children/1/layoutRows/0", "takes no `key`"},
        {"a block over a field that is no list",
         fixed("", R"("layoutRows":[{"forEach":"limits","rows":[{"key":"m","label":"M","cells":{}}]}])"),
         "/view/children/1/layoutRows/0/forEach", "is not a list of records of the form"},
        {"a block with no rows", fixed("", R"("layoutRows":[{"forEach":"specimens","rows":[]}])"),
         "/view/children/1/layoutRows/0/rows", "gives none"},
        {"a block inside a block",
         fixed("", R"("layoutRows":[{"forEach":"specimens","rows":[{"forEach":"specimens","rows":[]}]}])"),
         "/view/children/1/layoutRows/0/rows/0/forEach", "holds no block"},

        {"a row with no key", fixed("", R"("layoutRows":[{"label":"C","cells":{}}])"), "/view/children/1/layoutRows/0",
         "names its `key`"},
        {"a row that repeats rows", fixed("", R"("layoutRows":[{"key":"a","rows":[{"key":"b","cells":{}}]}])"),
         "/view/children/1/layoutRows/0/rows", "repeat for each row of a `forEach`"},
        {"a key holding a slash", fixed("", R"("layoutRows":[{"key":"a/b","cells":{}}])"),
         "/view/children/1/layoutRows/0/key", "holds a `/`"},
        {"a key declared twice", fixed("", R"("layoutRows":[{"key":"a","cells":{}},{"key":"a","cells":{}}])"),
         "/view/children/1/layoutRows/1/key", "is declared twice"},
        {"a cell under no column", fixed("", R"("layoutRows":[{"key":"a","cells":{"zz":{"text":"x"}}}])"), "",
         "is not one"},
        {"a cell that gives nothing", fixed("", R"("layoutRows":[{"key":"a","cells":{"min":{}}}])"), "",
         "one of them"},
        {"a cell that gives two",
         fixed("", R"("layoutRows":[{"key":"a","cells":{"min":{"field":"limits.count","text":"x"}}}])"), "",
         "one of them"},
        {"a field cell with no form",
         R"({"kind":"table","id":"g",)" + std::string{kColumns} +
             R"(,"layoutRows":[{"key":"a","cells":{"min":{"field":"limits.count"}}}]})",
         "", "give the table its `form`"},
        {"a field the form lacks", fixed("", R"("layoutRows":[{"key":"a","cells":{"min":{"field":"limits.nope"}}}])"),
         "", "is not a field of the form of"},
        {"a span of zero", fixed("", R"("layoutRows":[{"key":"a","cells":{"min":{"text":"x","columns":0}}}])"), "",
         "literal count"},
        {"a computed span",
         fixed("", R"("layoutRows":[{"key":"a","cells":{"min":{"text":"x","rows":{"fn":"coalesce","args":[1,2]}}}}])"),
         "", "literal count"},
        {"an overlay on a cell with no field",
         fixed("", R"("layoutRows":[{"key":"a","cells":{"min":{"text":"x","decimals":2}}}])"), "",
         "overlay of the field a cell edits"},
        {"a form grid over rows too",
         R"({"kind":"table","id":"g","form":"rec","field":"specimens","rows":{"lit":[]}})", "/view/children/1/form",
         "not both"},
        {"a form grid with no field", R"({"kind":"table","id":"g","form":"rec"})", "/view/children/1",
         "names its `field`"},
        {"a form grid over a missing form", R"({"kind":"table","id":"g","form":"nope","field":"specimens"})",
         "/view/children/1/form", "is not a form of this document"},
        {"a form grid with a bad enterMoves",
         R"({"kind":"table","id":"g","form":"rec","field":"specimens","enterMoves":"diagonal"})",
         "/view/children/1/enterMoves", "`down`, `right` or `none`"},
        {"a form grid with a bad walk",
         R"({"kind":"table","id":"g","form":"rec","field":"specimens","walk":"zigzag"})", "/view/children/1/walk",
         "`rows` or `columns`"},
        {"a form grid that appends and is fixed",
         R"({"kind":"table","id":"g","form":"rec","field":"specimens","appendRows":true,"rowsFixed":true})",
         "/view/children/1/appendRows", "adds no rows"},
        {"a form grid column with no member",
         R"({"kind":"table","id":"g","form":"rec","field":"specimens","columns":[{"id":"c","label":"C"}]})",
         "/view/children/1/columns/0", "edits the member its `field` names"},
        {"a form grid over a field that is no list",
         R"({"kind":"table","id":"g","form":"rec","field":"limits","columns":[{"id":"c","label":"C","field":"mass"}]})",
         "/view/children/1/field", "is not a list of records"},
        {"a form grid column naming no member",
         R"({"kind":"table","id":"g","form":"rec","field":"specimens","columns":[{"id":"c","label":"C","field":"nope"}]})",
         "/view/children/1/columns/0/field", "names no member of `specimens`"},
    };
    for (auto const& entry : cases) {
        CAPTURE(entry.name);
        auto const loaded = loadDocumentText(documentOf(entry.table), gridOptions(entry.schema));
        REQUIRE_FALSE(loaded.has_value());
        INFO(loaded.error().message());
        if (!entry.path.empty()) {
            CHECK(loaded.error().path == entry.path);
        }
        CHECK(loaded.error().reason.contains(entry.reason));
    }
}

TEST_CASE("grids over a form that are sound load, with a schema and without", "[covc][ui][document][grid]") {
    for (bool const schema : {true, false}) {
        for (
            std::string_view const table :
            {R"({"kind":"table","id":"g","form":"rec","field":"specimens","enterMoves":"right","walk":"rows",
                  "columns":[{"id":"c","label":"C","field":"mass"}]})",
             R"({"kind":"table","id":"g","form":"rec","field":"anything","columns":[{"id":"c","label":"C","field":"x"}]})"}) {
            auto const loaded = loadDocumentText(documentOf(table), gridOptions(schema));
            if (schema && table.contains("anything")) {
                CHECK_FALSE(loaded.has_value());
            } else if (!loaded) {
                FAIL(loaded.error().message());
            }
        }
    }
}

TEST_CASE("a fixed grid with sound rows loads", "[covc][ui][document][grid]") {
    auto const loaded = loadDocumentText(documentOf(fixed("")), gridOptions(true));
    if (!loaded) {
        FAIL(loaded.error().message());
    }
}

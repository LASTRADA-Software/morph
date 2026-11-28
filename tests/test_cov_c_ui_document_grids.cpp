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
        {.name = "rows and layoutRows",
         .table = fixed(R"("rows":{"lit":[]})"),
         .path = "/view/children/1/layoutRows",
         .reason = "in place of `rows`"},
        {.name = "a field of its own",
         .table = fixed(R"("field":"specimens")"),
         .path = "/view/children/1/field",
         .reason = "takes no `field`"},
        {.name = "a sort",
         .table = fixed(R"("sort":{"lit":"x"})"),
         .path = "/view/children/1/sort",
         .reason = "it takes no `sort`"},
        {.name = "row spans",
         .table = fixed(R"("rowSpans":{"lit":1})"),
         .path = "/view/children/1/rowSpans",
         .reason = "it takes no `rowSpans`"},
        {.name = "appended rows",
         .table = fixed(R"("appendRows":true)"),
         .path = "/view/children/1/appendRows",
         .reason = "it takes no `appendRows`"},
        {.name = "a key",
         .table = fixed(R"("key":"id")"),
         .path = "/view/children/1/key",
         .reason = "it takes no `key`"},
        {.name = "column given as an expression",
         .table =
             R"({"kind":"table","id":"g","form":"rec","columns":{"lit":1},"layoutRows":[{"key":"c","label":"C","cells":{}}]})",
         .path = "/view/children/1/columns",
         .reason = "a list of column parts"},
        {.name = "a column naming a field",
         .table =
             R"({"kind":"table","id":"g","form":"rec","columns":[{"id":"min","label":"Min","field":"x"}],"layoutRows":[{"key":"c","label":"C","cells":{}}]})",
         .path = "/view/children/1/columns/0",
         .reason = "name neither"},
        {.name = "a form that is not one",
         .table = fixed(R"("form":"nope")"),
         .path = "",
         .reason = "is not a form of this document"},
        {.name = "a block without a form",
         .table =
             R"({"kind":"table","id":"g",)" + std::string{kColumns} +
             R"(,"layoutRows":[{"forEach":"specimens","rows":[{"key":"m","label":"M","cells":{"min":{"text":"x"}}}]}]})",
         .path = "/view/children/1/layoutRows/0/forEach",
         .reason = "give the table its `form`"},
        {.name = "a block with a key",
         .table = fixed("", R"("layoutRows":[{"forEach":"specimens","key":"k","rows":[]}])"),
         .path = "/view/children/1/layoutRows/0",
         .reason = "takes no `key`"},
        {.name = "a block over a field that is no list",
         .table = fixed("", R"("layoutRows":[{"forEach":"limits","rows":[{"key":"m","label":"M","cells":{}}]}])"),
         .path = "/view/children/1/layoutRows/0/forEach",
         .reason = "is not a list of records of the form"},
        {.name = "a block with no rows",
         .table = fixed("", R"("layoutRows":[{"forEach":"specimens","rows":[]}])"),
         .path = "/view/children/1/layoutRows/0/rows",
         .reason = "gives none"},
        {.name = "a block inside a block",
         .table = fixed("", R"("layoutRows":[{"forEach":"specimens","rows":[{"forEach":"specimens","rows":[]}]}])"),
         .path = "/view/children/1/layoutRows/0/rows/0/forEach",
         .reason = "holds no block"},

        {.name = "a row with no key",
         .table = fixed("", R"("layoutRows":[{"label":"C","cells":{}}])"),
         .path = "/view/children/1/layoutRows/0",
         .reason = "names its `key`"},
        {.name = "a row that repeats rows",
         .table = fixed("", R"("layoutRows":[{"key":"a","rows":[{"key":"b","cells":{}}]}])"),
         .path = "/view/children/1/layoutRows/0/rows",
         .reason = "repeat for each row of a `forEach`"},
        {.name = "a key holding a slash",
         .table = fixed("", R"("layoutRows":[{"key":"a/b","cells":{}}])"),
         .path = "/view/children/1/layoutRows/0/key",
         .reason = "holds a `/`"},
        {.name = "a key declared twice",
         .table = fixed("", R"("layoutRows":[{"key":"a","cells":{}},{"key":"a","cells":{}}])"),
         .path = "/view/children/1/layoutRows/1/key",
         .reason = "is declared twice"},
        {.name = "a cell under no column",
         .table = fixed("", R"("layoutRows":[{"key":"a","cells":{"zz":{"text":"x"}}}])"),
         .path = "",
         .reason = "is not one"},
        {.name = "a cell that gives nothing",
         .table = fixed("", R"("layoutRows":[{"key":"a","cells":{"min":{}}}])"),
         .path = "",
         .reason = "one of them"},
        {.name = "a cell that gives two",
         .table = fixed("", R"("layoutRows":[{"key":"a","cells":{"min":{"field":"limits.count","text":"x"}}}])"),
         .path = "",
         .reason = "one of them"},
        {.name = "a field cell with no form",
         .table = R"({"kind":"table","id":"g",)" + std::string{kColumns} +
                  R"(,"layoutRows":[{"key":"a","cells":{"min":{"field":"limits.count"}}}]})",
         .path = "",
         .reason = "give the table its `form`"},
        {.name = "a field the form lacks",
         .table = fixed("", R"("layoutRows":[{"key":"a","cells":{"min":{"field":"limits.nope"}}}])"),
         .path = "",
         .reason = "is not a field of the form of"},
        {.name = "a span of zero",
         .table = fixed("", R"("layoutRows":[{"key":"a","cells":{"min":{"text":"x","columns":0}}}])"),
         .path = "",
         .reason = "literal count"},
        {.name = "a computed span",
         .table = fixed(
             "", R"("layoutRows":[{"key":"a","cells":{"min":{"text":"x","rows":{"fn":"coalesce","args":[1,2]}}}}])"),
         .path = "",
         .reason = "literal count"},
        {.name = "an overlay on a cell with no field",
         .table = fixed("", R"("layoutRows":[{"key":"a","cells":{"min":{"text":"x","decimals":2}}}])"),
         .path = "",
         .reason = "overlay of the field a cell edits"},
        {.name = "a form grid over rows too",
         .table = R"({"kind":"table","id":"g","form":"rec","field":"specimens","rows":{"lit":[]}})",
         .path = "/view/children/1/form",
         .reason = "not both"},
        {.name = "a form grid with no field",
         .table = R"({"kind":"table","id":"g","form":"rec"})",
         .path = "/view/children/1",
         .reason = "names its `field`"},
        {.name = "a form grid over a missing form",
         .table = R"({"kind":"table","id":"g","form":"nope","field":"specimens"})",
         .path = "/view/children/1/form",
         .reason = "is not a form of this document"},
        {.name = "a form grid with a bad enterMoves",
         .table = R"({"kind":"table","id":"g","form":"rec","field":"specimens","enterMoves":"diagonal"})",
         .path = "/view/children/1/enterMoves",
         .reason = "`down`, `right` or `none`"},
        {.name = "a form grid with a bad walk",
         .table = R"({"kind":"table","id":"g","form":"rec","field":"specimens","walk":"zigzag"})",
         .path = "/view/children/1/walk",
         .reason = "`rows` or `columns`"},
        {.name = "a form grid that appends and is fixed",
         .table = R"({"kind":"table","id":"g","form":"rec","field":"specimens","appendRows":true,"rowsFixed":true})",
         .path = "/view/children/1/appendRows",
         .reason = "adds no rows"},
        {.name = "a form grid column with no member",
         .table = R"({"kind":"table","id":"g","form":"rec","field":"specimens","columns":[{"id":"c","label":"C"}]})",
         .path = "/view/children/1/columns/0",
         .reason = "edits the member its `field` names"},
        {.name = "a form grid over a field that is no list",
         .table =
             R"({"kind":"table","id":"g","form":"rec","field":"limits","columns":[{"id":"c","label":"C","field":"mass"}]})",
         .path = "/view/children/1/field",
         .reason = "is not a list of records"},
        {.name = "a form grid column naming no member",
         .table =
             R"({"kind":"table","id":"g","form":"rec","field":"specimens","columns":[{"id":"c","label":"C","field":"nope"}]})",
         .path = "/view/children/1/columns/0/field",
         .reason = "names no member of `specimens`"},
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

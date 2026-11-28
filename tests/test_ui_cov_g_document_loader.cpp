// SPDX-License-Identifier: Apache-2.0

// The loader's refusals and near misses for keys whose literal shape is checked at load: each document breaks one rule
// and is refused at the key that breaks it, and the near miss beside it loads.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <morph/ui/document.hpp>
#include <string>
#include <string_view>

using morph::ui::loadDocumentText;

namespace {

struct Case {
    std::string_view name;
    std::string text;
    std::string_view expected;
};

std::string refusal(std::string const& text) {
    auto const loaded = loadDocumentText(text);
    return loaded ? std::string{"loads"} : loaded.error().path + ": " + loaded.error().reason;
}

std::string document(std::string_view view, std::string_view top = "") {
    return std::string{R"({"vocab": "ui/1", "id": "r", )"} + std::string{top} + R"( "view": )" + std::string{view} +
           "}";
}

std::string app(std::string_view shell) { return R"({"vocab":"ui/1","id":"app","shell":)" + std::string{shell} + "}"; }

std::string rowsTable(std::string_view extra, std::string_view columns = "") {
    return document(
        R"({"kind": "table", "id": "t", "rows": {"lit": []}, "columns": [{"id": "a", "label": "A", "sortable": true}, {"id": "b", "label": "B"})" +
        std::string{columns} + "]" + (extra.empty() ? "" : ", " + std::string{extra}) + "}");
}

std::string seventeen() {
    std::string columns;
    std::string ids;
    for (int index = 0; index < 17; ++index) {
        columns += R"(, {"id": "c)" + std::to_string(index) + R"(", "label": "C"})";
        ids += (index == 0 ? "" : ",") + std::string{"\"c"} + std::to_string(index) + "\"";
    }
    return rowsTable(R"("groups": [{"id": "g", "columns": {"lit": [)" + ids + R"(]}}])", columns);
}

}  // namespace

TEST_CASE("covg document loader: a literal that is the wrong shape is refused where it is written",
          "[covg][ui][document]") {
    auto const& c = GENERATE(values<Case>({
        {"a lead placement that is no place", document(R"({"kind": "spacer"})", R"("leadPlacement": "sideways",)"),
         "/leadPlacement: `leadPlacement` is `below`, `inline` or `end`"},
        {"a lead placement that is a number", document(R"({"kind": "spacer"})", R"("leadPlacement": 5,)"),
         "/leadPlacement: `leadPlacement` is `below`, `inline` or `end`"},
        {"a lead placement that is a place", document(R"({"kind": "spacer"})", R"("leadPlacement": "end",)"), "loads"},
        {"a prefetch list with an empty name", document(R"({"kind": "spacer"})", R"("prefetch": ["a", ""],)"),
         "/prefetch/1: a list of names is an array of strings"},
        {"a prefetch that is no list", document(R"({"kind": "spacer"})", R"("prefetch": "a",)"),
         "/prefetch: a list of names is an array of strings"},
        {"a shell that is no object", app("5"),
         "/shell: `shell` holds the parts: context, modules, search, inbox, principal, brand, help, searchHint"},
        {"an inbox with no source", app(R"({"inbox": {"screen": "s", "sources": []}})"),
         "/shell/inbox/sources: an inbox sums at least one source"},
        {"a page of a negative width", document(R"({"kind": "page", "width": -1, "height": 297000})"),
         "/view/width: `width` is a literal integer count of micrometres, 0 or more"},
        {"a page wider than a count of micrometres holds",
         document(R"({"kind": "page", "width": 3000000000, "height": 297000})"),
         "/view/width: `width` is a literal integer count of micrometres, 0 or more"},
        {"a band whose columns are computed",
         rowsTable(R"("bands": [{"label": "x", "columns": {"fn": "coalesce", "args": [{"lit": ["a"]}]}}])"),
         "/view/bands/0/columns: a band's `columns` is a literal list of its columns' ids"},
        {"a group of seventeen columns", seventeen(), "/view/groups/0/columns: a group has 2 to 16 columns"},
        {"a summary at the top", rowsTable(R"("summaries": [{"key": "s", "position": "top"}])"), "loads"},
        {"a table with no id that persists nothing",
         document(
             R"({"kind": "table", "rows": {"lit": []}, "columns": [{"id": "a", "label": "A"}], "persist": false})"),
         "loads"},
        {"a table with no id that persists under a name",
         document(
             R"({"kind": "table", "rows": {"lit": []}, "columns": [{"id": "a", "label": "A"}], "persist": "mine"})"),
         "/view/persist: a table over `rows` needs an `id` to persist its arrangement"},
        {"a select column whose mode is computed",
         rowsTable(R"("selectColumn": true, "selectionMode": {"fn": "coalesce", "args": ["multiple"]})"),
         "/view/selectColumn: a select column draws a table's selection of several rows; give it `selectionMode` "
         "`multiple`"},
        {"a column of a percent width", rowsTable("", R"(, {"id": "w", "label": "W", "width": "50%"})"), "loads"},
        {"a dialog sized by a fixed width",
         document(R"({"kind": "dialog", "sizing": {"lit": {"width": {"fixed": 3}}}})"), "loads"},
        {"a dialog sized by a fixed amount", document(R"({"kind": "dialog", "sizing": {"lit": {"fixed": 3}}})"),
         "loads"},
        {"a lead placement below", document(R"({"kind": "spacer"})", R"("leadPlacement": "below",)"), "loads"},
        {"a lead placement inline", document(R"({"kind": "spacer"})", R"("leadPlacement": "inline",)"), "loads"},
        {"a summary with an empty key", rowsTable(R"("summaries": [{"key": ""}])"),
         "/view/summaries/0/key: a summary names its `key`"},
        {"a summary at the bottom", rowsTable(R"("summaries": [{"key": "s", "position": "bottom"}])"), "loads"},
        {"a page stretched",
         document(R"({"kind": "page", "width": 210000, "height": 297000, "alignSelf": "stretch"})"),
         "/view/alignSelf: `alignSelf` places a node in its row, column or grid"},
        {"a page stretched in a column",
         document(
             R"({"kind": "column", "children": [{"kind": "page", "width": 210000, "height": 297000, "alignSelf": "stretch"}]})"),
         "/view/children/0/alignSelf: a page is never stretched; `alignSelf` places the sheet"},
        {"a page placed at the start",
         document(R"({"kind": "page", "width": 210000, "height": 297000, "alignSelf": "start"})"),
         "/view/alignSelf: `alignSelf` places a node in its row, column or grid"},
        {"a page that bleeds", document(R"({"kind": "page", "width": 210000, "height": 297000, "bleed": true})"),
         "/view/bleed: `bleed` is a child's of a column, a panel or a tab"},
        {"a locale scope with an id",
         document(R"({"kind": "localeScope", "locale": "de", "id": "x", "children": []})"),
         "/view/id: `id` is refused on a locale scope: it is not a widget; wrap it in a node that has one"},
        {"a locale scope with a place",
         document(R"({"kind": "localeScope", "locale": "de", "alignSelf": "start", "children": []})"),
         "/view/alignSelf: `alignSelf` places a node in its row, column or grid"},
        {"a locale scope that bleeds",
         document(R"({"kind": "localeScope", "locale": "de", "bleed": true, "children": []})"),
         "/view/bleed: `bleed` is a child's of a column, a panel or a tab"},
        {"a locale scope with a tooltip",
         document(R"({"kind": "localeScope", "locale": "de", "tooltip": "t", "children": []})"),
         "/view/tooltip: `tooltip` is refused on a locale scope: it is not a widget; wrap it in a node that has one"},
        {"a locale scope with a description of itself",
         document(R"({"kind": "localeScope", "locale": "de", "a11y": {"name": "n"}, "children": []})"),
         "/view/a11y: `a11y` is refused on a locale scope: it is not a widget; wrap it in a node that has one"},
        {"a dialog sized by content and a fixed height",
         document(R"({"kind": "dialog", "sizing": {"lit": {"width": "content", "height": {"fixed": 3}}}})"), "loads"},
        {"an align of a template's row",
         document(
             R"({"kind": "forEach", "rows": {"lit": [1]}, "children": [{"kind": "text", "text": "x", "alignSelf": "start"}]})"),
         "loads"},
        {"an align of a table's empty line",
         rowsTable(R"("empty": [{"kind": "text", "text": "x", "alignSelf": "start"}])"),
         "/view/empty/0/alignSelf: `alignSelf` places a node in its row, column or grid"},
        {"a sort change on a model table with no sortable column",
         document(
             R"({"kind": "table", "model": "sample", "action": "Lims_ListResults", "onSortChange": [{"notify": "x"}]})",
             R"("models": {"sample": {"type": "Lims_SampleModel"}},)"),
         "loads"},
        {"an edit mode on a model table named by its action",
         document(R"({"kind": "table", "model": "sample", "action": "Lims_ListResults", "editMode": "row"})",
                  R"("models": {"sample": {"type": "Lims_SampleModel"}},)"),
         "/view/editMode: `editMode` chooses how a model's table commits its cells; this table is not a model's "
         "table"},
        {"a literal filter on a table over rows", rowsTable(R"("filters": {"lit": {"columns": {}}})"),
         "/view/filters: `filters` belongs to a model-backed table; filter `rows` with `filter`"},
        {"a column of a percent width as a literal",
         rowsTable("", R"(, {"id": "w", "label": "W", "width": {"lit": {"percent": 50}}})"),
         "/view/columns/2/width: `percent` sizes a width along a row or a horizontal splitter; nothing here takes a "
         "share"},
        {"a percent width in a column",
         document(
             R"({"kind": "column", "children": [{"kind": "text", "text": "x", "sizing": {"lit": {"percent": 50}}}]})"),
         "/view/children/0/sizing: `percent` sizes a width along a row or a horizontal splitter; nothing here takes a "
         "share"},
        {"a percent width as the width of a record",
         document(
             R"({"kind": "column", "children": [{"kind": "text", "text": "x", "sizing": {"lit": {"width": {"percent": 50}}}}]})"),
         "/view/children/0/sizing: `percent` sizes a width along a row or a horizontal splitter; nothing here takes a "
         "share"},
        {"a percent width in a row",
         document(
             R"({"kind": "row", "children": [{"kind": "text", "text": "x", "sizing": {"lit": {"percent": 50}}}]})"),
         "loads"},
        {"a percent width in a wrapping row",
         document(
             R"({"kind": "row", "wrap": true, "children": [{"kind": "text", "text": "x", "sizing": {"lit": {"percent": 50}}}]})"),
         "/view/children/0/sizing: `percent` sizes a width along a row or a horizontal splitter; nothing here takes a "
         "share"},
        {"a percent width in a vertical splitter",
         document(
             R"({"kind": "splitter", "orientation": "vertical", "children": [{"kind": "text", "text": "x", "sizing": {"lit": {"percent": 50}}}, {"kind": "text", "text": "y"}]})"),
         "/view/children/0/sizing: `percent` sizes a width along a row or a horizontal splitter; nothing here takes a "
         "share"},
        {"a percent width in a horizontal splitter",
         document(
             R"({"kind": "splitter", "orientation": "horizontal", "children": [{"kind": "text", "text": "x", "sizing": {"lit": {"percent": 50}}}, {"kind": "text", "text": "y"}]})"),
         "loads"},
        {"a margin past the count a length holds",
         document(R"({"kind": "page", "width": 210000, "height": 297000, "margins": {"lit": {"top": 3000000000}}})"),
         "/view/margins: `margins` is a literal count of micrometres, or a record of `top`, `right`, `bottom` and "
         "`left` counts"},
        {"a margin below zero",
         document(R"({"kind": "page", "width": 210000, "height": 297000, "margins": {"lit": {"top": -1}}})"),
         "/view/margins: `margins` is a literal count of micrometres, or a record of `top`, `right`, `bottom` and "
         "`left` counts"},
        {"a margin of no side",
         document(R"({"kind": "page", "width": 210000, "height": 297000, "margins": {"lit": {"middle": 1}}})"),
         "/view/margins: `margins` is a literal count of micrometres, or a record of `top`, `right`, `bottom` and "
         "`left` counts"},
        {"a persist that names nothing", rowsTable(R"("persist": "")"),
         "/view/persist: `persist` is a literal: true, false or a name"},
        {"a literal filter object on a table over rows", rowsTable(R"("filters": {"columns": {}})"),
         "/view/filters: `filters` belongs to a model-backed table; filter `rows` with `filter`"},
        {"a literal filter of groups alone on a model table",
         document(R"({"kind": "table", "model": "sample", "action": "Lims_ListResults", "filters": {"groups": []}})",
                  R"("models": {"sample": {"type": "Lims_SampleModel"}},)"),
         "loads"},
        {"an empty filter object on a model table",
         document(R"({"kind": "table", "model": "sample", "action": "Lims_ListResults", "filters": {}})",
                  R"("models": {"sample": {"type": "Lims_SampleModel"}},)"),
         "/view/filters: an empty object is not an expression; a constant record is {\"lit\": {}}"},
    }));
    CAPTURE(c.name);
    CHECK(refusal(c.text) == c.expected);
}

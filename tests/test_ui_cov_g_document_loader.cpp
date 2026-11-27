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
    }));
    CAPTURE(c.name);
    CHECK(refusal(c.text) == c.expected);
}

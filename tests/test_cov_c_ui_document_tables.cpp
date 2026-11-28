// SPDX-License-Identifier: Apache-2.0
//
// Documents whose tables hold their own rows: summaries, bands, column groups, sorting, filtering, editing mode and
// the cell overlays. Each variant is refused with the path and the reason it breaks.

#include <catch2/catch_test_macros.hpp>
#include <morph/ui/document.hpp>
#include <string>
#include <string_view>
#include <vector>

using morph::ui::loadDocumentText;

namespace {

std::string documentOf(std::string_view node) {
    return R"({"vocab":"ui/1","id":"d","view":{"kind":"column","children":[)" + std::string{node} + "]}}";
}

// A document with a plain view and `extra` members beside it.
std::string rootWith(std::string_view extra) {
    return R"({"vocab":"ui/1","id":"d",)" + std::string{extra} + R"(,"view":{"kind":"text","text":"x"}})";
}

std::string columnWith(std::string_view extra) {
    return R"({"kind":"table","id":"t","rows":{"lit":[]},"columns":[{"id":"a","label":"A","value":{"lit":1},)" +
           std::string{extra} + "}]}";
}

std::string table(std::string_view extra) {
    return R"({"kind":"table","id":"t","rows":{"lit":[]},)"
           R"("columns":[{"id":"a","label":"A","value":{"lit":1}},{"id":"b","label":"B","value":{"lit":2}}])" +
           (extra.empty() ? std::string{} : "," + std::string{extra}) + "}";
}

struct Case {
    std::string name;
    std::string node;
    std::string path;
    std::string reason;
};

}  // namespace

TEST_CASE("a table over its own rows that breaks a rule of its parts is refused with its path",
          "[covc][ui][document][table]") {
    std::vector<Case> const cases{
        {.name = "a summary with no key",
         .node = table(R"("summaries":[{"cells":{}}])"),
         .path = "/view/children/0/summaries/0",
         .reason = "`key` is required"},
        {.name = "a summary twice",
         .node = table(R"("summaries":[{"key":"s"},{"key":"s"}])"),
         .path = "/view/children/0/summaries/1/key",
         .reason = "declared twice"},
        {.name = "a summary at the side",
         .node = table(R"("summaries":[{"key":"s","position":"left"}])"),
         .path = "/view/children/0/summaries/0/position",
         .reason = "`bottom` or `top`"},
        {.name = "a summary cell under no column",
         .node = table(R"("summaries":[{"key":"s","cells":{"zz":{"lit":1}}}])"),
         .path = "/view/children/0/summaries/0/cells/zz",
         .reason = "is not one"},
        {.name = "a band with no column list",
         .node = table(R"("bands":[{"label":"x"}])"),
         .path = "/view/children/0/bands/0",
         .reason = "`columns` is required"},
        {.name = "a band with a column that is no id",
         .node = table(R"("bands":[{"label":"x","columns":{"lit":[1]}}])"),
         .path = "/view/children/0/bands/0/columns",
         .reason = "literal list of its columns' ids"},
        {.name = "a band over a column the table lacks",
         .node = table(R"("bands":[{"label":"x","columns":{"lit":["zz"]}}])"),
         .path = "/view/children/0/bands",
         .reason = "band"},
        {.name = "bands with a child outside its parent",
         .node = table(
             R"("bands":[{"label":"x","columns":{"lit":["a"]},"bands":[{"label":"y","columns":{"lit":["b"]}}]}])"),
         .path = "/view/children/0/bands",
         .reason = "its parent band does not"},
        {.name = "a group with no column list",
         .node = table(R"("groups":[{"id":"g"}])"),
         .path = "/view/children/0/groups/0",
         .reason = "`columns` is required"},
        {.name = "a group of one column",
         .node = table(R"("groups":[{"id":"g","columns":{"lit":["a"]}}])"),
         .path = "/view/children/0/groups/0/columns",
         .reason = "a group"},
        {.name = "a group of unknown columns",
         .node = table(R"("groups":[{"id":"g","columns":{"lit":["a","zz"]}}])"),
         .path = "/view/children/0/groups/0/columns",
         .reason = "a group's columns"},
        {.name = "a group declared twice",
         .node = table(R"("groups":[{"id":"g","columns":{"lit":["a","b"]}},{"id":"g","columns":{"lit":["a","b"]}}])"),
         .path = "/view/children/0/groups/1/id",
         .reason = "declared twice"},
        {.name = "a column in two groups",
         .node = table(R"("groups":[{"id":"g","columns":{"lit":["a","b"]}},{"id":"h","columns":{"lit":["a","b"]}}])"),
         .path = "/view/children/0/groups/1/columns",
         .reason = "is in another group"},
        {.name = "a computed sort change",
         .node = table(R"("onSortChange":[{"set":"x","value":1}])"),
         .path = "/view/children/0/onSortChange/0",
         .reason = ""},
        {.name = "an edit mode on a table over rows",
         .node = table(R"("editMode":"row")"),
         .path = "/view/children/0/editMode",
         .reason = "not a model's table"},
        {.name = "a bad filters literal",
         .node = table(R"("filters":{"lit":5})"),
         .path = "/view/children/0/filters",
         .reason = "belongs"},
        {.name = "persist as a computed value",
         .node = table(R"("persist":{"fn":"coalesce","args":[true]})"),
         .path = "/view/children/0/persist",
         .reason = "`persist` is"},
        {.name = "a cell mode off a grid",
         .node = table(R"("selectionMode":"cells")"),
         .path = "/view/children/0/selectionMode",
         .reason = ""},
        {.name = "a badge that can be removed and cannot",
         .node = R"({"kind":"badge","text":{"lit":"x"},"removeLabel":{"lit":"x"}})",
         .path = "/view/children/0/removeLabel",
         .reason = ""},
        {.name = "a forEach with a computed axis",
         .node =
             R"({"kind":"forEach","items":{"lit":[]},"axis":{"fn":"coalesce","args":["row"]},"template":{"kind":"text","text":{"lit":"x"}}})",
         .path = "",
         .reason = ""},
        {.name = "an align of a child outside a place",
         .node = R"({"kind":"panel","children":[{"kind":"text","text":{"lit":"x"},"alignSelf":"start"}]})",
         .path = "/view/children/0/children/0/alignSelf",
         .reason = ""},
        {.name = "a bleeding child outside a column",
         .node = R"({"kind":"row","children":[{"kind":"text","text":{"lit":"x"},"bleed":true}]})",
         .path = "/view/children/0/children/0/bleed",
         .reason = ""},
        {.name = "a divided horizontal forEach",
         .node =
             R"({"kind":"forEach","items":{"lit":[]},"as":"x","axis":"horizontal","divided":true,"rows":{"kind":"text","text":{"lit":"x"}}})",
         .path = "",
         .reason = ""},
        {.name = "a wrapping vertical forEach",
         .node =
             R"({"kind":"forEach","items":{"lit":[]},"as":"x","axis":"vertical","wrap":true,"rows":{"kind":"text","text":{"lit":"x"}}})",
         .path = "",
         .reason = ""},
        {.name = "a table with no id that persists",
         .node =
             R"({"kind":"table","rows":{"lit":[]},"columns":[{"id":"a","label":"A","value":{"lit":1}}],"persist":true})",
         .path = "/view/children/0/persist",
         .reason = "needs an `id`"},
        {.name = "a highlight key on a rows table",
         .node = table(R"("highlightKey":{"lit":"x"})"),
         .path = "/view/children/0/highlightKey",
         .reason = "links a model-backed table to a chart"},
        {.name = "a column of no known kind",
         .node =
             R"({"kind":"table","id":"t","rows":{"lit":[]},"columns":[{"id":"a","label":"A","value":{"lit":1},"kind":"blob"}]})",
         .path = "/view/children/0/columns/0/kind",
         .reason = "text, integer, decimal"},
        {.name = "a select column with no multiple selection",
         .node = table(R"("selectColumn":true,"selectionMode":"single")"),
         .path = "/view/children/0/selectColumn",
         .reason = "`multiple`"},
        {.name = "a select-all label with no select column",
         .node = table(R"("selectAllLabel":{"lit":"x"})"),
         .path = "/view/children/0/selectAllLabel",
         .reason = "give the table `selectColumn`"},
        {.name = "an append flag on a table that is no grid",
         .node = table(R"("appendRows":true)"),
         .path = "/view/children/0/appendRows",
         .reason = "is a grid's"},
        {.name = "a page with no page size",
         .node = table(R"("page":{"lit":1})"),
         .path = "/view/children/0/page",
         .reason = "give the table `pageSize`"},
        {.name = "a page size of zero",
         .node = table(R"("pageSize":0)"),
         .path = "/view/children/0/pageSize",
         .reason = "positive whole number"},
        {.name = "a sort bound of zero",
         .node = table(R"("maxSortKeys":0)"),
         .path = "/view/children/0/maxSortKeys",
         .reason = "1 to 32"},
        {.name = "a sort bound over 32",
         .node = table(R"("maxSortKeys":33)"),
         .path = "/view/children/0/maxSortKeys",
         .reason = "1 to 32"},
        {.name = "a computed configurable",
         .node = table(R"("configurable":{"fn":"coalesce","args":[true]})"),
         .path = "/view/children/0/configurable",
         .reason = "literal `true`, `false`"},
        {.name = "a configurable number",
         .node = table(R"("configurable":5)"),
         .path = "/view/children/0/configurable",
         .reason = "literal `true`, `false`"},
        {.name = "a configurable list of numbers",
         .node = table(R"("configurable":{"lit":["hide",5]})"),
         .path = "/view/children/0/configurable",
         .reason = "literal `true`, `false`"},
        {.name = "a configurable list naming zoom",
         .node = table(R"("configurable":{"lit":["zoom"]})"),
         .path = "/view/children/0/configurable",
         .reason = "names `zoom`"},
        {.name = "a selection with no selection mode",
         .node = table(R"("selection":{"lit":null})"),
         .path = "/view/children/0/selection",
         .reason = "`single` or `multiple`"},
        {.name = "a selection report with the mode none",
         .node = table(R"("onSelectionChange":[{"set":"x","value":1}],"selectionMode":"none")"),
         .path = "",
         .reason = ""},
        {.name = "a grouped pair of columns of two kinds",
         .node =
             R"({"kind":"table","id":"t","rows":{"lit":[]},"columns":[{"id":"a","label":"A","value":{"lit":1},"kind":"text"},)"
             R"({"id":"b","label":"B","value":{"lit":2},"kind":"integer"}],"groups":[{"id":"g","columns":{"lit":["a","b"]}}]})",
         .path = "/view/children/0/groups/0/columns",
         .reason = "hold one kind"},
        {.name = "tabs with a twice-used id",
         .node =
             R"({"kind":"tabs","tabs":[{"id":"a","label":"A","children":[]},{"id":"a","label":"B","children":[]}]})",
         .path = "",
         .reason = "used twice"},
        {.name = "tabs selecting no tab",
         .node = R"({"kind":"tabs","selected":{"lit":"zz"},"tabs":[{"id":"a","label":"A","children":[]}]})",
         .path = "",
         .reason = "names no tab"},
        {.name = "a tab with no id under a selection",
         .node = R"({"kind":"tabs","selected":{"lit":"a"},"tabs":[{"label":"A","children":[]}]})",
         .path = "",
         .reason = "has an `id`"},
        {.name = "tabs with an inset beyond five",
         .node = R"({"kind":"tabs","inset":6,"tabs":[{"id":"a","label":"A","children":[]}]})",
         .path = "",
         .reason = "spacing step from 0 to 5"},
        {.name = "a page with a margin side that is no side",
         .node = R"({"kind":"page","width":210000,"height":297000,"margins":{"lit":{"middle":1}},"children":[]})",
         .path = "",
         .reason = "`margins` is a literal count"},
        {.name = "a page with a negative margin",
         .node = R"({"kind":"page","width":210000,"height":297000,"margins":{"lit":{"left":-1}},"children":[]})",
         .path = "",
         .reason = "`margins` is a literal count"},
        {.name = "a page with a margin that is no count",
         .node = R"({"kind":"page","width":210000,"height":297000,"margins":{"lit":{"top":"x"}},"children":[]})",
         .path = "",
         .reason = "`margins` is a literal count"},
        {.name = "a page with a computed margin",
         .node =
             R"({"kind":"page","width":210000,"height":297000,"margins":{"fn":"coalesce","args":[1]},"children":[]})",
         .path = "",
         .reason = "micrometres"},
        {.name = "a page whose width is negative",
         .node = R"({"kind":"page","width":-5,"height":297000,"children":[]})",
         .path = "",
         .reason = "micrometres"},
        {.name = "spans that are no list",
         .node = rootWith(R"("titleSpans":{"lit":5})"),
         .path = "",
         .reason = "`spans` is a list of runs"},
        {.name = "a run that is a number",
         .node = rootWith(R"("titleSpans":{"lit":[5]})"),
         .path = "",
         .reason = "a run is a string or a record"},
        {.name = "a run with no text",
         .node = rootWith(R"("titleSpans":{"lit":[{"strong":true}]})"),
         .path = "",
         .reason = "gives its `text`"},
        {.name = "a run with an unknown flag",
         .node = rootWith(R"("titleSpans":{"lit":[{"text":"a","bold":true}]})"),
         .path = "",
         .reason = "a run takes `text`"},
        {.name = "a run with a flag that is no boolean",
         .node = rootWith(R"("titleSpans":{"lit":[{"text":"a","strong":"yes"}]})"),
         .path = "",
         .reason = "is true or false"},
        {.name = "a run with an unknown tone",
         .node = rootWith(R"("titleSpans":{"lit":[{"text":"a","tone":"loud"}]})"),
         .path = "",
         .reason = "neutral, info, ok, warn or err"},
        {.name = "a run that takes the action outside a banner",
         .node = rootWith(R"("titleSpans":{"lit":[{"text":"a","action":true}]})"),
         .path = "",
         .reason = "link of a banner"},
        {.name = "a listed run with no text",
         .node = rootWith(R"("subtitleSpans":{"list":[{"obj":{"strong":{"lit":true}}}]})"),
         .path = "",
         .reason = "gives its `text`"},
        {.name = "a listed run with an unknown key",
         .node = rootWith(R"("subtitleSpans":{"list":[{"obj":{"text":"a","zz":1}}]})"),
         .path = "",
         .reason = "a run takes `text`"},
        {.name = "a listed run with a flag that is no boolean",
         .node = rootWith(R"("eyebrowSpans":{"list":[{"obj":{"text":"a","strong":1}}]})"),
         .path = "",
         .reason = "is true or false"},
        {.name = "a listed run with an unknown tone",
         .node = rootWith(R"("eyebrowSpans":{"list":[{"obj":{"text":"a","tone":"loud"}}]})"),
         .path = "",
         .reason = "neutral, info, ok, warn or err"},
        {.name = "a listed run that takes the action outside a banner",
         .node = rootWith(R"("eyebrowSpans":{"list":[{"obj":{"text":"a","action":true}}]})"),
         .path = "",
         .reason = "link of a banner"},
        {.name = "a column with a readonly overlay",
         .node = columnWith(R"("readonly":true)"),
         .path = "",
         .reason = "is a grid column's"},
        {.name = "a column with a field as an expression",
         .node = columnWith(R"("field":{"lit":"x"})"),
         .path = "",
         .reason = "as an expression"},
        {.name = "a column with decimals beyond 18",
         .node = columnWith(R"("decimals":40)"),
         .path = "",
         .reason = "0 to 18"},
        {.name = "a column with unknown rounding",
         .node = columnWith(R"("rounding":"floor")"),
         .path = "",
         .reason = "halfEven or halfUp"},
        {.name = "a column with a date pattern that is none",
         .node = columnWith(R"("date":"qq")"),
         .path = "",
         .reason = ""},
        {.name = "a column with labels that are no list",
         .node = columnWith(R"("labels":5)"),
         .path = "",
         .reason = "literal list"},
        {.name = "a column with a label missing its text",
         .node = columnWith(R"("labels":{"lit":[{"operand":"a"}]})"),
         .path = "",
         .reason = "literal list"},
        {.name = "a column placed at stretch", .node = columnWith(R"("align":"stretch")"), .path = "", .reason = ""},
        {.name = "a column valigned at stretch",
         .node = columnWith(R"("valign":"stretch")"),
         .path = "",
         .reason = ""},
        {.name = "a multiline input with a unit",
         .node = R"({"kind":"textInput","testId":"i","mode":"multiline","unit":"kg"})",
         .path = "",
         .reason = "shows no unit"},
        {.name = "a single-line input that wraps",
         .node = R"({"kind":"textInput","testId":"i","wrap":true})",
         .path = "",
         .reason = "has no lines to wrap"},
        {.name = "a table that persists under a bad name",
         .node = table(R"("persist":5)"),
         .path = "/view/children/0/persist",
         .reason = "literal: true, false or a name"},
        {.name = "a table filter that is no filter",
         .node = table(R"("filters":{"columns":5})"),
         .path = "/view/children/0/filters",
         .reason = "is not a filter"},
        {.name = "a node with a negative bound",
         .node = R"({"kind":"text","text":"x","minWidth":-1})",
         .path = "",
         .reason = "0 or more"},
        {.name = "a node with a computed bound",
         .node = R"({"kind":"text","text":"x","maxHeight":{"fn":"coalesce","args":[1]}})",
         .path = "",
         .reason = "literal integer of 0 or more"},
        {.name = "a node whose minimum passes its maximum",
         .node = R"({"kind":"text","text":"x","minWidth":5,"maxWidth":3})",
         .path = "",
         .reason = "is more than"},
        {.name = "an overlay with a bound",
         .node = R"({"kind":"dialog","title":"x","minWidth":3,"children":[]})",
         .path = "",
         .reason = "sized by the frontend"},
        {.name = "an overlay that stretches",
         .node = R"({"kind":"dialog","title":"x","sizing":{"lit":{"stretch":1}},"children":[]})",
         .path = "",
         .reason = "sized by the frontend"},
        {.name = "a separator that carries more",
         .node = R"({"kind":"menuItem","separator":true,"label":"x"})",
         .path = "",
         .reason = ""},
        {.name = "a margin that is no record",
         .node = R"({"kind":"page","margins":{"lit":5},"children":[]})",
         .path = "",
         .reason = ""},
    };
    for (auto const& entry : cases) {
        CAPTURE(entry.name);
        auto const loaded =
            loadDocumentText(entry.node.starts_with(R"({"vocab")") ? entry.node : documentOf(entry.node));
        if (loaded.has_value()) {
            FAIL_CHECK("loaded");
            continue;
        }
        INFO(loaded.error().message());
        UNSCOPED_INFO("REASON " << entry.name << " | " << loaded.error().path << " | " << loaded.error().reason);
        if (!entry.path.empty()) {
            CHECK(loaded.error().path == entry.path);
        }
        CHECK(loaded.error().reason.contains(entry.reason));
    }
}

TEST_CASE("a table over its own rows with sound parts loads", "[covc][ui][document][table]") {
    auto const loaded =
        loadDocumentText(documentOf(table(R"("summaries":[{"key":"s","position":"top","cells":{"a":{"lit":1}}}],)"
                                          R"("bands":[{"label":"x","columns":{"lit":["a","b"]}}],)"
                                          R"("groups":[{"id":"g","columns":{"lit":["a","b"]}}])")));
    if (!loaded) {
        FAIL(loaded.error().message());
    }
}

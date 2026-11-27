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
        {"a summary with no key", table(R"("summaries":[{"cells":{}}])"), "/view/children/0/summaries/0",
         "`key` is required"},
        {"a summary twice", table(R"("summaries":[{"key":"s"},{"key":"s"}])"), "/view/children/0/summaries/1/key",
         "declared twice"},
        {"a summary at the side", table(R"("summaries":[{"key":"s","position":"left"}])"),
         "/view/children/0/summaries/0/position", "`bottom` or `top`"},
        {"a summary cell under no column", table(R"("summaries":[{"key":"s","cells":{"zz":{"lit":1}}}])"),
         "/view/children/0/summaries/0/cells/zz", "is not one"},
        {"a band with no column list", table(R"("bands":[{"label":"x"}])"), "/view/children/0/bands/0",
         "`columns` is required"},
        {"a band with a column that is no id", table(R"("bands":[{"label":"x","columns":{"lit":[1]}}])"),
         "/view/children/0/bands/0/columns", "literal list of its columns' ids"},
        {"a band over a column the table lacks", table(R"("bands":[{"label":"x","columns":{"lit":["zz"]}}])"),
         "/view/children/0/bands", "band"},
        {"bands with a child outside its parent",
         table(R"("bands":[{"label":"x","columns":{"lit":["a"]},"bands":[{"label":"y","columns":{"lit":["b"]}}]}])"),
         "/view/children/0/bands", "its parent band does not"},
        {"a group with no column list", table(R"("groups":[{"id":"g"}])"), "/view/children/0/groups/0",
         "`columns` is required"},
        {"a group of one column", table(R"("groups":[{"id":"g","columns":{"lit":["a"]}}])"),
         "/view/children/0/groups/0/columns", "a group"},
        {"a group of unknown columns", table(R"("groups":[{"id":"g","columns":{"lit":["a","zz"]}}])"),
         "/view/children/0/groups/0/columns", "a group's columns"},
        {"a group declared twice",
         table(R"("groups":[{"id":"g","columns":{"lit":["a","b"]}},{"id":"g","columns":{"lit":["a","b"]}}])"),
         "/view/children/0/groups/1/id", "declared twice"},
        {"a column in two groups",
         table(R"("groups":[{"id":"g","columns":{"lit":["a","b"]}},{"id":"h","columns":{"lit":["a","b"]}}])"),
         "/view/children/0/groups/1/columns", "is in another group"},
        {"a computed sort change", table(R"("onSortChange":[{"set":"x","value":1}])"),
         "/view/children/0/onSortChange/0", ""},
        {"an edit mode on a table over rows", table(R"("editMode":"row")"), "/view/children/0/editMode",
         "not a model's table"},
        {"a bad filters literal", table(R"("filters":{"lit":5})"), "/view/children/0/filters", "belongs"},
        {"persist as a computed value", table(R"("persist":{"fn":"coalesce","args":[true]})"),
         "/view/children/0/persist", "`persist` is"},
        {"a cell mode off a grid", table(R"("selectionMode":"cells")"), "/view/children/0/selectionMode", ""},
        {"a badge that can be removed and cannot", R"({"kind":"badge","text":{"lit":"x"},"removeLabel":{"lit":"x"}})",
         "/view/children/0/removeLabel", ""},
        {"a forEach with a computed axis",
         R"({"kind":"forEach","items":{"lit":[]},"axis":{"fn":"coalesce","args":["row"]},"template":{"kind":"text","text":{"lit":"x"}}})",
         "", ""},
        {"an align of a child outside a place",
         R"({"kind":"panel","children":[{"kind":"text","text":{"lit":"x"},"alignSelf":"start"}]})",
         "/view/children/0/children/0/alignSelf", ""},
        {"a bleeding child outside a column",
         R"({"kind":"row","children":[{"kind":"text","text":{"lit":"x"},"bleed":true}]})",
         "/view/children/0/children/0/bleed", ""},
        {"a divided horizontal forEach",
         R"({"kind":"forEach","items":{"lit":[]},"as":"x","axis":"horizontal","divided":true,"rows":{"kind":"text","text":{"lit":"x"}}})",
         "", ""},
        {"a wrapping vertical forEach",
         R"({"kind":"forEach","items":{"lit":[]},"as":"x","axis":"vertical","wrap":true,"rows":{"kind":"text","text":{"lit":"x"}}})",
         "", ""},
        {"a table with no id that persists",
         R"({"kind":"table","rows":{"lit":[]},"columns":[{"id":"a","label":"A","value":{"lit":1}}],"persist":true})",
         "/view/children/0/persist", "needs an `id`"},
        {"a highlight key on a rows table", table(R"("highlightKey":{"lit":"x"})"), "/view/children/0/highlightKey",
         "links a model-backed table to a chart"},
        {"a column of no known kind",
         R"({"kind":"table","id":"t","rows":{"lit":[]},"columns":[{"id":"a","label":"A","value":{"lit":1},"kind":"blob"}]})",
         "/view/children/0/columns/0/kind", "text, integer, decimal"},
        {"a select column with no multiple selection", table(R"("selectColumn":true,"selectionMode":"single")"),
         "/view/children/0/selectColumn", "`multiple`"},
        {"a select-all label with no select column", table(R"("selectAllLabel":{"lit":"x"})"),
         "/view/children/0/selectAllLabel", "give the table `selectColumn`"},
        {"an append flag on a table that is no grid", table(R"("appendRows":true)"), "/view/children/0/appendRows",
         "is a grid's"},
        {"a page with no page size", table(R"("page":{"lit":1})"), "/view/children/0/page",
         "give the table `pageSize`"},
        {"a page size of zero", table(R"("pageSize":0)"), "/view/children/0/pageSize", "positive whole number"},
        {"a sort bound of zero", table(R"("maxSortKeys":0)"), "/view/children/0/maxSortKeys", "1 to 32"},
        {"a sort bound over 32", table(R"("maxSortKeys":33)"), "/view/children/0/maxSortKeys", "1 to 32"},
        {"a computed configurable", table(R"("configurable":{"fn":"coalesce","args":[true]})"),
         "/view/children/0/configurable", "literal `true`, `false`"},
        {"a configurable number", table(R"("configurable":5)"), "/view/children/0/configurable",
         "literal `true`, `false`"},
        {"a configurable list of numbers", table(R"("configurable":{"lit":["hide",5]})"),
         "/view/children/0/configurable", "literal `true`, `false`"},
        {"a configurable list naming zoom", table(R"("configurable":{"lit":["zoom"]})"),
         "/view/children/0/configurable", "names `zoom`"},
        {"a selection with no selection mode", table(R"("selection":{"lit":null})"), "/view/children/0/selection",
         "`single` or `multiple`"},
        {"a selection report with the mode none",
         table(R"("onSelectionChange":[{"set":"x","value":1}],"selectionMode":"none")"), "", ""},
        {"a grouped pair of columns of two kinds",
         R"({"kind":"table","id":"t","rows":{"lit":[]},"columns":[{"id":"a","label":"A","value":{"lit":1},"kind":"text"},)"
         R"({"id":"b","label":"B","value":{"lit":2},"kind":"integer"}],"groups":[{"id":"g","columns":{"lit":["a","b"]}}]})",
         "/view/children/0/groups/0/columns", "hold one kind"},
        {"tabs with a twice-used id",
         R"({"kind":"tabs","tabs":[{"id":"a","label":"A","children":[]},{"id":"a","label":"B","children":[]}]})", "",
         "used twice"},
        {"tabs selecting no tab",
         R"({"kind":"tabs","selected":{"lit":"zz"},"tabs":[{"id":"a","label":"A","children":[]}]})", "",
         "names no tab"},
        {"a tab with no id under a selection",
         R"({"kind":"tabs","selected":{"lit":"a"},"tabs":[{"label":"A","children":[]}]})", "", "has an `id`"},
        {"tabs with an inset beyond five",
         R"({"kind":"tabs","inset":6,"tabs":[{"id":"a","label":"A","children":[]}]})", "", "spacing step from 0 to 5"},
        {"a page with a margin side that is no side",
         R"({"kind":"page","width":210000,"height":297000,"margins":{"lit":{"middle":1}},"children":[]})", "",
         "`margins` is a literal count"},
        {"a page with a negative margin",
         R"({"kind":"page","width":210000,"height":297000,"margins":{"lit":{"left":-1}},"children":[]})", "",
         "`margins` is a literal count"},
        {"a page with a margin that is no count",
         R"({"kind":"page","width":210000,"height":297000,"margins":{"lit":{"top":"x"}},"children":[]})", "",
         "`margins` is a literal count"},
        {"a page with a computed margin",
         R"({"kind":"page","width":210000,"height":297000,"margins":{"fn":"coalesce","args":[1]},"children":[]})", "",
         "micrometres"},
        {"a page whose width is negative", R"({"kind":"page","width":-5,"height":297000,"children":[]})", "",
         "micrometres"},
        {"a margin that is no record", R"({"kind":"page","margins":{"lit":5},"children":[]})", "", ""},
    };
    for (auto const& entry : cases) {
        CAPTURE(entry.name);
        auto const loaded = loadDocumentText(documentOf(entry.node));
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

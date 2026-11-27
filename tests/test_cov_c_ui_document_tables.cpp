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

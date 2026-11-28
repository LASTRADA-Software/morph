// SPDX-License-Identifier: Apache-2.0

// A table's element values, read by name from a document, are the same whatever backs the table: each kind of table
// answers the names it has, and a name it has not is null.

#include <catch2/catch_test_macros.hpp>
#include <morph/forms/engine/form_session.hpp>
#include <string>
#include <string_view>
#include <vector>

#include "ui_table_support.hpp"

namespace {
using namespace morph;
using morph::ui::testing::kModelTable;
using morph::ui::testing::kThree;
using morph::ui::testing::readJson;
using morph::ui::testing::replyDescribe;
using morph::ui::testing::rowsDoc;
using morph::ui::testing::TableHarness;

constexpr std::string_view kRowsSchema = R"({"type":"object","title":"Rows","properties":{
  "rows":{"type":"array","title":"Rows","x-order":0,"items":{"type":"object","properties":{
    "mass":{"type":["object","null"],"title":"Mass","x-decimalPlaces":3,"x-order":0},
    "note":{"type":["string","null"],"title":"Note","x-order":2}}}}}})";

constexpr std::string_view kGrid = R"({"vocab":"ui/1","id":"rows","models":{"sample":{"type":"Lims_SampleModel"}},
  "view":{"kind":"column","children":[
    {"kind":"form","id":"rec","model":"sample","action":"Lims_RecordRows","submit":"explicit"},
    {"kind":"table","id":"t","testId":"t","form":"rec","field":"rows","columns":[{"id":"mass","field":"mass","label":"Mass"},{"id":"note","field":"note","label":"Note"}]}]}})";

// NOLINTNEXTLINE(cert-err58-cpp,bugprone-throwing-static-initialization): a test table; an allocation failure at start-up ends the run either way.
std::vector<std::string_view> const kNames = {
    "layout",  "visibleColumns", "hiddenCount", "filters",      "modifiedCount", "modifiedRows",      "errorCount",
    "editing", "selection",      "busyCount",   "selectionSet", "selectedCount", "selectedCountKind", "activeKey",
    "sort",    "viewCount",      "page",        "pageCount",    "pageFirst",     "pageLast",          "sourceCount",
    "pending", "error",          "failed",      "nothing",      "visible",       "enabled",           "valid"};

struct Expect {
    std::string_view name;
    std::string_view value;
};

// NOLINTNEXTLINE(cert-err58-cpp,bugprone-throwing-static-initialization): a test table; an allocation failure at start-up ends the run either way.
std::vector<Expect> const kRows = {
    {"layout", R"({"hidden":[],"order":["id","name","weight"],"pinned":0,"widths":{}})"},
    {"visibleColumns", R"(["id","name","weight"])"},
    {"hiddenCount", R"(0)"},
    {"filters", R"({"columns":{},"groups":[]})"},
    {"modifiedCount", R"(0)"},
    {"modifiedRows", R"(0)"},
    {"errorCount", R"(0)"},
    {"editing", R"(null)"},
    {"selection", R"([])"},
    {"busyCount", R"(0)"},
    {"selectionSet", R"({"keys":[]})"},
    {"selectedCount", R"(0)"},
    {"selectedCountKind", R"("exact")"},
    {"activeKey", R"(null)"},
    {"sort", R"([])"},
    {"viewCount", R"(3)"},
    {"page", R"(0)"},
    {"pageCount", R"(1)"},
    {"pageFirst", R"(1)"},
    {"pageLast", R"(3)"},
    {"sourceCount", R"(3)"},
    {"pending", R"(false)"},
    {"error", R"(null)"},
    {"failed", R"(null)"},
    {"nothing", R"(null)"},
    {"visible", R"(true)"},
    {"enabled", R"(true)"},
    {"valid", R"(true)"},
};

// NOLINTNEXTLINE(cert-err58-cpp,bugprone-throwing-static-initialization): a test table; an allocation failure at start-up ends the run either way.
std::vector<Expect> const kModel = {
    {"layout", R"({"hidden":[],"order":["id","name","c3"],"pinned":0,"widths":{}})"},
    {"visibleColumns", R"(["id","name","c3"])"},
    {"hiddenCount", R"(0)"},
    {"filters", R"({"columns":{},"groups":[]})"},
    {"modifiedCount", R"(0)"},
    {"modifiedRows", R"(0)"},
    {"errorCount", R"(0)"},
    {"editing", R"(null)"},
    {"selection", R"([])"},
    {"busyCount", R"(0)"},
    {"selectionSet", R"({"keys":[]})"},
    {"selectedCount", R"(0)"},
    {"selectedCountKind", R"("exact")"},
    {"activeKey", R"(null)"},
    {"sort", R"([])"},
    {"viewCount", R"(null)"},
    {"page", R"(0)"},
    {"pageCount", R"(1)"},
    {"pageFirst", R"(0)"},
    {"pageLast", R"(0)"},
    {"sourceCount", R"(0)"},
    {"pending", R"(true)"},
    {"error", R"(null)"},
    {"failed", R"([])"},
    {"nothing", R"(null)"},
    {"visible", R"(true)"},
    {"enabled", R"(true)"},
    {"valid", R"(true)"},
};

// NOLINTNEXTLINE(cert-err58-cpp,bugprone-throwing-static-initialization): a test table; an allocation failure at start-up ends the run either way.
std::vector<Expect> const kGridValues = {
    {"layout", R"({"hidden":[],"order":["mass","note"],"pinned":0,"widths":{}})"},
    {"visibleColumns", R"(["mass","note"])"},
    {"hiddenCount", R"(0)"},
    {"filters", R"({"columns":{},"groups":[]})"},
    {"modifiedCount", R"(0)"},
    {"modifiedRows", R"(0)"},
    {"errorCount", R"(0)"},
    {"editing", R"(null)"},
    {"selection", R"(null)"},
    {"busyCount", R"(0)"},
    {"selectionSet", R"({"keys":[]})"},
    {"selectedCount", R"(0)"},
    {"selectedCountKind", R"("exact")"},
    {"activeKey", R"(null)"},
    {"sort", R"([])"},
    {"viewCount", R"(0)"},
    {"page", R"(0)"},
    {"pageCount", R"(1)"},
    {"pageFirst", R"(0)"},
    {"pageLast", R"(0)"},
    {"sourceCount", R"(0)"},
    {"pending", R"(false)"},
    {"error", R"(null)"},
    {"failed", R"(null)"},
    {"nothing", R"(null)"},
    {"visible", R"(true)"},
    {"enabled", R"(true)"},
    {"valid", R"(true)"},
};

void check(TableHarness& h, std::unique_ptr<ui::Screen> const& screen, std::string_view id,
           std::vector<Expect> const& expected) {
    static_cast<void>(h);
    for (Expect const& e : expected) {
        CAPTURE(e.name);
        CHECK(readJson(*screen, R"({"ref":")" + std::string{id} + "." + std::string{e.name} + R"("})") == e.value);
    }
}

}  // namespace

TEST_CASE("covg element values: a table over rows answers the names a table has", "[covg][ui][table]") {
    TableHarness h;
    auto screen = h.mount(rowsDoc(kThree));
    check(h, screen, "list", kRows);
}

TEST_CASE("covg element values: a model table that has not been answered yet is pending", "[covg][ui][table]") {
    TableHarness h;
    auto screen = h.mount(std::string{kModelTable});
    replyDescribe(h);
    check(h, screen, "list", kModel);
}

TEST_CASE("covg element values: a form grid answers its edits and its selection", "[covg][ui][table]") {
    TableHarness h;
    ui::testing::addAction(h, "Lims_RecordRows", std::string{kRowsSchema},
                           ui::ActionInfo{.modelType = "Lims_SampleModel"});
    auto screen = h.mount(std::string{kGrid});
    ui::testing::elementOf(*screen, "rec").form->prefill(R"({"rows":[{},{}]})");
    h.settle();
    check(h, screen, "t", kGridValues);
}

TEST_CASE("covg element values: a selection of rows and the busy rows of a table are counted", "[covg][ui][table]") {
    TableHarness h;
    std::string const text = rowsDoc(kThree, R"(,"selectionMode":"multiple")");
    auto screen = h.mount(text);
    int const table = h.backend.all("Table").front();
    h.backend.clickRow(table, ui::Key{std::string{"a"}});
    h.backend.ctrlClickRow(table, ui::Key{std::string{"c"}});
    h.settle();
    CHECK(readJson(*screen, R"({"ref":"list.selectedCount"})") == "2");
    CHECK(readJson(*screen, R"({"ref":"list.selectedCountKind"})") == R"("exact")");
    CHECK(readJson(*screen, R"({"ref":"list.selection"})") == R"(["a","c"])");
    CHECK(readJson(*screen, R"({"ref":"list.selectionSet"})") == R"({"keys":["a","c"]})");
}

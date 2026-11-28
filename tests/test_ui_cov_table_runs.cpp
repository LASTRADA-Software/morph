// SPDX-License-Identifier: Apache-2.0

// A model table's observer of the session's runs: the rows a run names are busy, a reply's failed rows are marked,
// and a key a reply moves is followed by the selection of the view, the retained rows and the status line.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <memory>
#include <morph/table/description.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ui_table_model_support.hpp"
#include "ui_table_support.hpp"

namespace {

using namespace table_model_support;
using morph::ui::Value;
using morph::ui::testing::addAction;
using morph::ui::testing::describeJson;
using morph::ui::testing::elementValue;
using morph::ui::testing::refuseMutation;
using morph::ui::testing::replyDescribe;
using morph::ui::testing::replyPage;
using morph::ui::testing::runMutation;
using morph::ui::testing::settleMutation;
using morph::ui::testing::statusOf;
using morph::ui::testing::TableHarness;

constexpr std::string_view kActingTable =
    R"({"kind":"table","id":"t","testId":"t","model":"sample","table":"samples","selectionMode":"multiple"})";

constexpr std::string_view kRetaining =
    R"({"kind":"table","id":"t","testId":"t","model":"sample","table":"samples","selectionMode":"multiple",
        "retainChanged":true})";

constexpr std::string_view kMutations = R"({
    "save":{"model":"sample","action":"Lims_SaveAs","body":{"obj":{"rows":{"ref":"t.selectionSet"}}}},
    "touch":{"model":"sample","action":"Lims_TouchRow","body":{"obj":{"row":{"ref":"picked"}}}}})";

std::vector<SelRow> tenRows() {
    std::vector<SelRow> rows;
    rows.reserve(10);
    for (std::int64_t key = 0; key < 10; ++key) {
        rows.push_back(SelRow{.key = key, .status = key < 6 ? "open" : "closed", .lockedBy = {}});
    }
    return rows;
}

std::string document(TableHarness& h, std::string_view tableNode) {
    h.mutationActions.insert_or_assign("save", "Lims_SaveAs");
    h.mutationActions.insert_or_assign("touch", "Lims_TouchRow");
    std::string children{tableNode};
    for (std::string const name : {"save", "touch"}) {
        children += R"(,{"kind":"button","testId":"run-)";
        children += name;
        children += R"(","label":")";
        children += name;
        children += R"(","onClick":[{"run":")";
        children += name;
        children += R"("}]})";
    }
    for (int const key : {2, 4}) {
        std::string const text = std::to_string(key);
        children += R"(,{"kind":"button","testId":"pick-)";
        children += text;
        children += R"(","label":"pick )";
        children += text;
        children += R"(","onClick":[{"set":"picked","value":{"lit":)";
        children += text;
        children += "}}]}";
    }
    return R"({"vocab":"ui/1","id":"samples","models":{"sample":{"type":"Lims_SampleModel"}},)"
           R"("state":{"picked":{"init":{"lit":null}}},"mutations":)" +
           std::string{kMutations} + R"(,"view":{"kind":"column","children":[)" + children + "]}}";
}

std::unique_ptr<morph::ui::Screen> mount(TableHarness& h, std::string_view tableNode = kActingTable) {
    addAction(h, "Lims_SaveAs", std::string{kSelectionSchema}, samplesAction());
    addAction(h, "Lims_TouchRow", std::string{kRowSchema}, samplesAction());
    auto screen = h.mount(document(h, tableNode));
    replyDescribe(h, describeJson(selectionDescription()));
    replyPage(h, selectionPage(tenRows(), 10, "v1"));
    return screen;
}

void pick(TableHarness& h, int key) {
    h.backend.click(h.byTestId("Button", "pick-" + std::to_string(key)));
    h.settle();
}

std::string changed(std::string_view rows, std::string_view failed = "") {
    return R"({"rows":{"rows":[)" + std::string{rows} + R"(],"failed":[)" + std::string{failed} + "]}}";
}
}  // namespace

TEST_CASE("cov table runs: a failed row without a message is marked and the next run over it clears the mark",
          "[ui][table][busy][cov]") {
    TableHarness h;
    auto screen = mount(h);
    morph::ui::TableRows& rows = *morph::ui::testing::elementOf(*screen, "t").tableRows;
    pick(h, 4);
    runMutation(h, "touch");
    settleMutation(h, "touch", changed("", R"({"key":4})"));
    REQUIRE(rows.states().failure(table::RowId{std::int64_t{4}}).has_value());
    CHECK(rows.states().failure(table::RowId{std::int64_t{4}})->empty());
    runMutation(h, "touch");  // running again on the row clears its mark while it runs
    CHECK_FALSE(rows.states().failure(table::RowId{std::int64_t{4}}).has_value());
    settleMutation(h, "touch", changed(R"({"key":4})"));
    CHECK_FALSE(rows.states().failure(table::RowId{std::int64_t{4}}).has_value());
    // A refused run of an action with no catalog entry for the table's record settles without a reply to read.
    runMutation(h, "touch");
    refuseMutation(h, "touch", "lims.rows.denied");
    CHECK_FALSE(rows.states().isBusy(table::RowId{std::int64_t{4}}));
}

TEST_CASE("cov table runs: a selection of the view follows a key a reply moves", "[ui][table][keys][cov]") {
    TableHarness h;
    auto screen = mount(h);
    int const table = h.byTestId("Table", "t");
    h.backend.selectAll(table);
    h.settle();
    h.backend.ctrlClickRow(table, std::int64_t{3});
    h.backend.ctrlClickRow(table, std::int64_t{9});
    h.settle();
    REQUIRE(elementValue(h, "t", "selectionSet").field("except") != nullptr);
    CHECK(*elementValue(h, "t", "selectionSet").field("except") == value("[3,9]"));
    runMutation(h, "save");
    // Row 3 is left out: its new key is left out in its place, and 2, which nothing holds, does not move.
    settleMutation(h, "save", changed(R"({"key":30,"from":3},{"key":20,"from":2})"));
    CHECK(*elementValue(h, "t", "selectionSet").field("except") == value("[30,9]"));
    // A key left out that moves onto another left out leaves one.
    runMutation(h, "save");
    settleMutation(h, "save", changed(R"({"key":9,"from":30})"));
    CHECK(*elementValue(h, "t", "selectionSet").field("except") == value("[9]"));
    runMutation(h, "save");
    settleMutation(h, "save", changed(R"({"key":5,"from":9})"));
    CHECK(*elementValue(h, "t", "selectionSet").field("except") == value("[5]"));
}

TEST_CASE("cov table runs: a reply naming more rows than a table keeps says so", "[ui][table][retain][cov]") {
    TableHarness h;
    auto const screen = mount(h, kRetaining);
    int const table = h.byTestId("Table", "t");
    h.backend.clickRow(table, std::int64_t{2});
    h.settle();
    runMutation(h, "save");
    std::string rows;
    for (std::int64_t key = 100; key < 100 + 10'001; ++key) {
        rows += (rows.empty() ? "" : ",") + std::string{R"({"key":)"} + std::to_string(key) + "}";
    }
    settleMutation(h, "save", changed(rows));
    CHECK(statusOf(h, table).message == "The changed rows are not kept in view: a table keeps at most 10,000");
    // A handful of rows is kept, and an empty reply keeps none.
    runMutation(h, "save");
    settleMutation(h, "save", changed(R"({"key":2},{"key":4})"));
    CHECK(morph::ui::testing::elementOf(*screen, "t").tableWindow->retained().size() == 2);
    runMutation(h, "save");
    settleMutation(h, "save", changed(""));
    CHECK(morph::ui::testing::elementOf(*screen, "t").tableWindow->retained().size() == 2);
}

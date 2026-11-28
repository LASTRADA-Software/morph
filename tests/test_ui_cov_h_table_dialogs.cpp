// SPDX-License-Identifier: Apache-2.0

// What the column chooser offers for a layout, which groups the chooser may form, and how the sort dialog's edits
// change a chain: each case is a layout or a chain and the exact answer.

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <morph/table/engine.hpp>
#include <morph/ui/table_config.hpp>
#include <morph/ui/table_dialogs.hpp>
#include <optional>
#include <string>
#include <vector>

namespace {

namespace table = morph::table;
namespace ui = morph::ui;

std::vector<ui::LayoutColumn> columnsOf(const std::vector<std::string>& ids) {
    std::vector<ui::LayoutColumn> out;
    for (auto& id : ids) {
        out.push_back(
            ui::LayoutColumn{.id = id, .label = "Label " + id, .locked = false, .kind = table::ColumnKind::Text});
    }
    return out;
}

table::SortChain chainOf(std::vector<std::pair<std::string, table::SortDirection>> keys) {
    table::SortChain out;
    for (auto& [column, direction] : keys) {
        out.push_back(table::SortKey{.column = column, .dir = direction});
    }
    return out;
}

std::vector<std::string> columnsIn(table::SortChain const& chain) {
    std::vector<std::string> out;
    for (auto const& key : chain) {
        out.push_back(key.column);
    }
    return out;
}

constexpr auto kUp = table::SortDirection::Ascending;
constexpr auto kDown = table::SortDirection::Descending;

}  // namespace

TEST_CASE("cov h dialogs: the chooser offers what the layout and the capabilities allow", "[covh][ui][table]") {
    struct Case {
        char const* what;
        ui::Configure allowed;
        std::size_t pinned;
        std::vector<std::string> hidden;
        bool lockB;
        // one string per column in order: shown, toggles, up, down
        std::vector<std::string> expected;
    };
    for (Case const& item : std::vector<Case>{
             {"every capability",
              ui::Configure::All,
              0,
              {},
              false,
              {"shown toggles down", "shown toggles up down", "shown toggles up"}},
             {"reordering alone", ui::Configure::Reorder, 0, {}, false, {"shown down", "shown up down", "shown up"}},
             {"hiding alone", ui::Configure::Hide, 0, {}, false, {"shown toggles", "shown toggles", "shown toggles"}},
             {"pinning alone", ui::Configure::Pin, 0, {}, false, {"shown", "shown", "shown"}},
             {"a pin after the first column",
              ui::Configure::All,
              1,
              {},
              false,
              {"shown toggles", "shown toggles down", "shown toggles up"}},
             {"a pin between the last two",
              ui::Configure::All,
              2,
              {},
              false,
              {"shown toggles down", "shown toggles up", "shown toggles"}},
             {"a hidden column",
              ui::Configure::All,
              0,
              {"b"},
              false,
              {"shown toggles down", "toggles", "shown toggles up"}},
             {"a locked column", ui::Configure::All, 0, {}, true, {"shown toggles down", "shown", "shown toggles up"}},
         }) {
        INFO(item.what);
        table::ColumnLayout layout{{"a", "b", "c"}, item.hidden};
        layout.setPinned(item.pinned);
        auto columns = columnsOf({"a", "b", "c"});
        columns.at(1).locked = item.lockB;
        auto const state = ui::chooserState(layout, columns, item.allowed);
        REQUIRE(state.columns.size() == item.expected.size());
        for (std::size_t at = 0; at < item.expected.size(); ++at) {
            auto const& column = state.columns.at(at);
            std::string got;
            for (auto const& [flag, name] : std::vector<std::pair<bool, char const*>>{
                     {column.shown, "shown"}, {column.toggles, "toggles"}, {column.up, "up"}, {column.down, "down"}}) {
                if (flag) {
                    got += (got.empty() ? "" : " ") + std::string{name};
                }
            }
            INFO("column " << column.id);
            CHECK(got == item.expected.at(at));
        }
    }
}

TEST_CASE("cov h dialogs: the last shown column cannot be hidden, and a layout column the table lacks is not listed",
          "[covh][ui][table]") {
    table::ColumnLayout const layout{{"a", "b", "z"}, {"b", "z"}};
    auto const columns = columnsOf({"a", "b"});
    auto const state = ui::chooserState(layout, columns, ui::Configure::All);
    REQUIRE(state.columns.size() == 2);  // `z` is in no column of the table
    CHECK(state.columns.at(0).shown);
    CHECK_FALSE(state.columns.at(0).toggles);  // `a` is the only column shown
    CHECK_FALSE(state.columns.at(1).shown);
    CHECK(state.columns.at(1).toggles);
}

TEST_CASE("cov h dialogs: a group is named, of 2 to the most columns", "[covh][ui][table]") {
    struct Case {
        char const* label;
        std::size_t ticked;
        bool expected;
    };
    for (Case const& item : std::vector<Case>{{"Group", 2, true},
                                              {"Group", 1, false},
                                              {"Group", 0, false},
                                              {"", 3, false},
                                              {" \t", 3, false},
                                              {" x ", 3, true},
                                              {"Group", ui::kMaxGroupColumns, true},
                                              {"Group", ui::kMaxGroupColumns + 1, false}}) {
        ui::GroupDraft draft;
        draft.label = item.label;
        draft.ticked.assign(item.ticked, "c");
        INFO(item.label << " with " << item.ticked);
        CHECK(ui::canFormGroup(draft) == item.expected);
    }
}

TEST_CASE("cov h dialogs: the sort dialog's edits change a chain or leave it", "[covh][ui][table]") {
    std::vector<std::string> const sortable{"a", "b", "c"};
    auto const edit = [&](table::SortChain const& chain, ui::SortEdit const& what, std::size_t bound = 4) {
        return ui::applySortEdit(chain, sortable, what, bound);
    };
    auto const twoKeys = chainOf({{"a", kUp}, {"b", kDown}});

    // Add: the first sortable column the chain lacks, ascending; none left or the bound reached leaves the chain.
    CHECK(columnsIn(edit(twoKeys, ui::AddSortKey{}).chain) == std::vector<std::string>{"a", "b", "c"});
    auto const full = chainOf({{"a", kUp}, {"b", kUp}, {"c", kUp}});
    auto const none = edit(full, ui::AddSortKey{});
    CHECK(none.chain == full);
    CHECK_FALSE(none.refusal.has_value());
    auto const bound = edit(twoKeys, ui::AddSortKey{}, 2);
    CHECK(bound.chain == twoKeys);
    CHECK(bound.refusal.has_value());

    // An index past the chain changes nothing, whatever the edit.
    for (ui::SortEdit const& past : std::vector<ui::SortEdit>{
             ui::SetSortColumn{.index = 2, .column = "c"}, ui::SetSortDirection{.index = 2, .direction = kDown},
             ui::MoveSortKey{.index = 2, .by = -1}, ui::RemoveSortKey{.index = 2}}) {
        CHECK(edit(twoKeys, past).chain == twoKeys);
    }

    // Remove.
    CHECK(columnsIn(edit(twoKeys, ui::RemoveSortKey{.index = 0}).chain) == std::vector<std::string>{"b"});

    // A direction already held changes nothing; another is the header's extend gesture.
    CHECK(edit(twoKeys, ui::SetSortDirection{.index = 0, .direction = kUp}).chain == twoKeys);
    auto const flipped = edit(twoKeys, ui::SetSortDirection{.index = 0, .direction = kDown}).chain;
    REQUIRE(flipped.size() == 2);
    CHECK(flipped.at(0).dir == kDown);

    // Moving swaps with the neighbour, and stops at either end or for a step of none.
    CHECK(columnsIn(edit(twoKeys, ui::MoveSortKey{.index = 0, .by = 1}).chain) == std::vector<std::string>{"b", "a"});
    CHECK(columnsIn(edit(twoKeys, ui::MoveSortKey{.index = 1, .by = -1}).chain) == std::vector<std::string>{"b", "a"});
    CHECK(edit(twoKeys, ui::MoveSortKey{.index = 0, .by = -1}).chain == twoKeys);
    CHECK(edit(twoKeys, ui::MoveSortKey{.index = 1, .by = 1}).chain == twoKeys);
    CHECK(edit(twoKeys, ui::MoveSortKey{.index = 0, .by = 0}).chain == twoKeys);

    // Setting a key's column: a column that sorts and no other key holds replaces it.
    CHECK(columnsIn(edit(twoKeys, ui::SetSortColumn{.index = 1, .column = "c"}).chain) ==
          std::vector<std::string>{"a", "c"});
    CHECK(columnsIn(edit(twoKeys, ui::SetSortColumn{.index = 1, .column = "b"}).chain) ==
          std::vector<std::string>{"a", "b"});
    CHECK(edit(twoKeys, ui::SetSortColumn{.index = 1, .column = "a"}).chain == twoKeys);   // another key holds it
    CHECK(edit(twoKeys, ui::SetSortColumn{.index = 1, .column = "zz"}).chain == twoKeys);  // does not sort
}

TEST_CASE("cov h dialogs: each key of the chain offers the columns no other key holds", "[covh][ui][table]") {
    std::vector<ui::SortColumnOption> const sortable{
        {.id = "a", .label = "A"}, {.id = "b", .label = "B"}, {.id = "c", .label = "C"}};
    auto const keys = ui::sortDialogKeys(chainOf({{"a", kUp}, {"c", kDown}}), sortable);
    REQUIRE(keys.size() == 2);
    CHECK_FALSE(keys.at(0).up);
    CHECK(keys.at(0).down);
    CHECK(keys.at(1).up);
    CHECK_FALSE(keys.at(1).down);
    REQUIRE(keys.at(0).columns.size() == 2);  // a and b; c is the second key's
    CHECK(keys.at(0).columns.at(1).label == "B");
    REQUIRE(keys.at(1).columns.size() == 2);  // b and c
    CHECK(keys.at(1).columns.at(0).label == "B");
    CHECK(ui::sortDialogKeys({}, sortable).empty());
}

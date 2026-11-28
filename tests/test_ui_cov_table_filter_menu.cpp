// SPDX-License-Identifier: Apache-2.0

// The filter menu's pure helpers, and the menu itself driven over rows that answer the values list from a script:
// what each tick, search, apply and clear writes to the table's filter.

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <morph/core/completion.hpp>
#include <morph/reactive/signal.hpp>
#include <morph/table/data_source.hpp>
#include <morph/table/filter.hpp>
#include <morph/table/query.hpp>
#include <morph/testing/step_executor.hpp>
#include <morph/ui/table_filter_menu.hpp>
#include <morph/ui/table_rows.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ui_table_support.hpp"

namespace {

namespace ui = morph::ui;
namespace table = morph::table;
namespace fm = morph::ui::detail::filtermenu;
using table::ColumnKind;
using DistinctReply = std::expected<table::DistinctValues, table::TableError>;

table::DistinctValue valueOf(std::string text, std::string operand, std::int64_t count = 1) {
    return table::DistinctValue{.text = std::move(text), .operand = std::move(operand), .count = count};
}

table::ColumnFilter oneOfFilter(std::vector<std::string> members, bool exclude = false) {
    table::FilterEntry entry;
    entry.oneOf = std::move(members);
    table::ColumnFilter filter;
    (exclude ? filter.exclude : filter.include).push_back(std::move(entry));
    return filter;
}

// Rows that list no cells and answer a values list from a function.
class ScriptedRows final : public ui::TableRows {
public:
    explicit ScriptedRows(morph::exec::IExecutor& owner) : _owner{&owner} {}

    // NOLINTBEGIN(cppcoreguidelines-non-private-member-variables-in-classes): a test double the test steers directly.
    std::function<DistinctReply(table::DistinctRequest const&)> answer;
    std::vector<table::DistinctRequest> requests;
    // NOLINTEND(cppcoreguidelines-non-private-member-variables-in-classes)

    [[nodiscard]] ui::TableTotal total() const override { return {}; }
    [[nodiscard]] std::optional<ui::TableRowRef> row(std::int64_t) const override { return std::nullopt; }
    [[nodiscard]] table::UnloadedLevels unloadedLevels(std::int64_t) const override { return {}; }
    void visible(std::int64_t, std::int64_t, std::span<std::string const>) override {}
    morph::async::Completion<std::expected<std::vector<table::RowId>, table::TableError>> keysOf(
        std::int64_t, std::int64_t) override {
        return morph::async::Completion<std::expected<std::vector<table::RowId>, table::TableError>>::makeSettleable(
                   _owner)
            .first;
    }
    void locateRow(table::RowId, ::core::async::StopToken, std::function<void(ui::RowLocation const&)>) override {}
    morph::async::Completion<std::expected<std::vector<ui::FetchedRow>, table::TableError>> fetchRows(
        std::vector<table::RowId>, std::vector<std::string>) override {
        return morph::async::Completion<std::expected<std::vector<ui::FetchedRow>, table::TableError>>::makeSettleable(
                   _owner)
            .first;
    }
    std::expected<void, table::TableError> setSort(ui::SortChain) override { return {}; }
    std::expected<void, table::TableError> setFilter(table::FilterSpec) override { return {}; }
    morph::async::Completion<DistinctReply> distinctValues(table::DistinctRequest request) override {
        requests.push_back(request);
        auto [completion, promise] = morph::async::Completion<DistinctReply>::makeSettleable(_owner);
        promise.resolve(answer(request));
        return std::move(completion);
    }
    [[nodiscard]] ui::TableCountsView counts() const override { return {}; }
    [[nodiscard]] ui::RowStates& states() override { return _states; }
    [[nodiscard]] ui::RowStates const& states() const override { return _states; }
    void moveKey(table::RowId const&, table::RowId const&) override {}
    void onChange(std::function<void(table::ViewChange const&)>) override {}

private:
    morph::exec::IExecutor* _owner;
    ui::RowStates _states;
};

// A page of @p all by the request's offset and limit.
std::function<DistinctReply(table::DistinctRequest const&)> pagesOf(std::vector<table::DistinctValue> all,
                                                                    std::int64_t blanks = 0) {
    return [all = std::move(all), blanks](table::DistinctRequest const& request) -> DistinctReply {
        table::DistinctValues out;
        std::vector<table::DistinctValue> matching;
        for (auto const& value : all) {
            if (request.search.empty() || value.text.contains(request.search) ||
                value.operand.starts_with(request.search)) {
                matching.push_back(value);
            }
        }
        for (auto at = static_cast<std::size_t>(request.offset);
             at < matching.size() && std::cmp_less(out.values.size(), request.limit); ++at) {
            out.values.push_back(matching.at(at));
        }
        out.distinct = static_cast<std::int64_t>(matching.size());
        out.blanks = blanks;
        return out;
    };
}

struct Menu {
    morph::testing::StepExecutor owner;
    morph::reactive::Runtime runtime{owner};
    ScriptedRows rows{owner};
    table::FilterSpec spec;
    std::vector<table::FilterSpec> applied;
    std::vector<std::vector<std::string>> listings;
    bool accept = true;
    int touches = 0;
    ui::FilterMenu menu{runtime};
    ui::FilterMenuHost host;

    Menu() {
        host.rows = [this] { return &rows; };
        host.filters = [this] { return spec; };
        host.apply = [this](table::FilterSpec next) {
            applied.push_back(next);
            if (accept) {
                spec = std::move(next);
            }
            return accept;
        };
        host.touch = [this] { ++touches; };
        host.listing = [this](std::vector<std::string> const& columns) { listings.push_back(columns); };
        host.format = morph::ui::testing::formatFor("en");
        menu.setHost(host);
    }
    void settle() { owner.runAll(); }
    ui::FilterMenuState const& state() const { return menu.state(); }

    void open(const std::string& column, ColumnKind kind = ColumnKind::Text) {
        ui::FilterTarget target;
        target.column = column;
        target.label = column;
        target.shown = table::TableColumn{.info = {.id = column, .kind = kind}, .label = column};
        menu.open(std::move(target));
        settle();
    }
    void openGroup(const std::string& group, std::vector<std::string> columns, bool formed = false) {
        ui::FilterTarget target;
        target.group = group;
        target.columns = std::move(columns);
        target.formed = formed;
        target.label = group;
        target.shown = table::TableColumn{.info = {.id = "g", .kind = ColumnKind::Text}, .label = "g"};
        menu.open(std::move(target));
        settle();
    }
    std::vector<std::string> labels() const {
        std::vector<std::string> out;
        for (auto const& row : menu.rows()) {
            out.push_back(row.label);
        }
        return out;
    }
};

std::vector<table::DistinctValue> letters(std::size_t n) {
    std::vector<table::DistinctValue> out;
    for (std::size_t at = 0; at < n; ++at) {
        std::string const name{static_cast<char>('a' + at)};
        out.push_back(valueOf(name, name, static_cast<std::int64_t>(at + 1)));
    }
    return out;
}
}  // namespace

TEST_CASE("cov filter menu: folded lowercases ASCII and dayOf reads only a real calendar day",
          "[ui][table][filter_menu][cov]") {
    CHECK(fm::folded("AbC-Z9") == "abc-z9");
    CHECK(fm::dayOf("1970-01-01") == 0);
    CHECK(fm::dayOf("2024-10-04T08:00:00Z") == 20'000);
    CHECK_FALSE(fm::dayOf("2024-10").has_value());
    CHECK_FALSE(fm::dayOf("2024/10/04").has_value());
    CHECK_FALSE(fm::dayOf("2024-10-xx").has_value());
    CHECK_FALSE(fm::dayOf("xxxx-10-04").has_value());
    CHECK_FALSE(fm::dayOf("2023-02-30").has_value());
    CHECK_FALSE(fm::dayOf("2023-13-01").has_value());
}

TEST_CASE("cov filter menu: ranges parse, print and cover each other", "[ui][table][filter_menu][cov]") {
    auto const range = fm::rangeOf("2024-01-01..2024-01-31");
    REQUIRE(range.has_value());
    CHECK(range->second - range->first == 30);
    CHECK_FALSE(fm::rangeOf("2024-01-01").has_value());
    CHECK_FALSE(fm::rangeOf("bad..2024-01-31").has_value());
    CHECK_FALSE(fm::rangeOf("2024-01-01..bad").has_value());
    CHECK(fm::rangeText(range->first, range->second, false) == "2024-01-01..2024-01-31");
    CHECK(fm::rangeText(range->first, range->second, true) == "2024-01-01T00:00:00Z..2024-01-31T23:59:59Z");

    std::vector<std::string> const held{"2024-01-01..2024-12-31", "2023-05-05..2023-05-06"};
    auto const inside = fm::coverOf(held, "2024-03-01..2024-03-31");
    CHECK(inside.first);
    CHECK_FALSE(inside.second);
    auto const own = fm::coverOf(held, "2023-05-05..2023-05-06");
    CHECK(own.first);
    CHECK(own.second);
    auto const outside = fm::coverOf(held, "2025-01-01..2025-01-31");
    CHECK_FALSE(outside.first);
    CHECK_FALSE(outside.second);
    // A bucket that is no range is covered only by itself.
    CHECK(fm::coverOf(std::vector<std::string>{"x"}, "x").second);
    CHECK_FALSE(fm::coverOf(held, "x").first);
}

TEST_CASE("cov filter menu: a one-operator entry is a condition draft", "[ui][table][filter_menu][cov]") {
    using ui::ConditionOp;
    auto const draftOfEntry = [](auto set) {
        table::FilterEntry entry;
        set(entry);
        return fm::draftOf(entry);
    };
    CHECK(draftOfEntry([](table::FilterEntry& e) { e.eq = "a"; })->op == ConditionOp::Equals);
    CHECK(draftOfEntry([](table::FilterEntry& e) { e.ne = "a"; })->op == ConditionOp::NotEquals);
    CHECK(draftOfEntry([](table::FilterEntry& e) { e.contains = "a"; })->op == ConditionOp::Contains);
    CHECK(draftOfEntry([](table::FilterEntry& e) { e.startsWith = "a"; })->op == ConditionOp::BeginsWith);
    CHECK(draftOfEntry([](table::FilterEntry& e) { e.endsWith = "a"; })->op == ConditionOp::EndsWith);
    CHECK(draftOfEntry([](table::FilterEntry& e) { e.lt = "1"; })->op == ConditionOp::Less);
    CHECK(draftOfEntry([](table::FilterEntry& e) { e.le = "1"; })->op == ConditionOp::LessOrEqual);
    CHECK(draftOfEntry([](table::FilterEntry& e) { e.gt = "1"; })->op == ConditionOp::Greater);
    CHECK(draftOfEntry([](table::FilterEntry& e) { e.ge = "1"; })->op == ConditionOp::GreaterOrEqual);
    auto const between = draftOfEntry([](table::FilterEntry& e) { e.between = std::array<std::string, 2>{"1", "9"}; });
    REQUIRE(between.has_value());
    CHECK(between->op == ConditionOp::Between);
    CHECK(between->operand == "1");
    CHECK(between->second == "9");
    CHECK(draftOfEntry([](table::FilterEntry& e) { e.isEmpty = true; })->op == ConditionOp::IsEmpty);
    CHECK(draftOfEntry([](table::FilterEntry& e) { e.notEmpty = true; })->op == ConditionOp::IsNotEmpty);
    CHECK_FALSE(draftOfEntry([](table::FilterEntry&) {}).has_value());
    CHECK_FALSE(draftOfEntry([](table::FilterEntry& e) { e.isEmpty = false; }).has_value());
    CHECK_FALSE(draftOfEntry([](table::FilterEntry& e) { e.oneOf = std::vector<std::string>{"a"}; }).has_value());
    CHECK_FALSE(draftOfEntry([](table::FilterEntry& e) {
                    e.eq = "a";
                    e.ne = "b";
                }).has_value());
}

TEST_CASE("cov filter menu: ranges merge, filters read back as ticks and drafts", "[ui][table][filter_menu][cov]") {
    std::vector<std::string> const ticked{"2024-01-01..2024-01-31", "2024-02-01..2024-02-29", "bad",
                                          "2024-05-01..2024-05-31", "2024-05-10..2024-05-12"};
    CHECK(ui::mergeDateRanges(ticked, false) ==
          std::vector<std::string>{"2024-01-01..2024-02-29", "2024-05-01..2024-05-31"});
    CHECK(ui::mergeDateRanges(std::vector<std::string>{"2024-01-01..2024-01-02"}, true) ==
          std::vector<std::string>{"2024-01-01T00:00:00Z..2024-01-02T23:59:59Z"});
    CHECK(ui::mergeDateRanges({}, false).empty());

    auto include = oneOfFilter({"a", "b"});
    table::FilterEntry blank;
    blank.isEmpty = true;
    include.include.push_back(blank);
    auto const inc = ui::ticksOfFilter(include);
    CHECK_FALSE(inc.ticked);
    CHECK(inc.blanks == true);
    REQUIRE(inc.ops.size() == 2);
    CHECK(inc.ops.front().operand == "a");
    CHECK(inc.ops.front().ticked);
    auto const exc = ui::ticksOfFilter(oneOfFilter({"x"}, true));
    CHECK(exc.ticked);
    CHECK(exc.blanks == true);
    REQUIRE(exc.ops.size() == 1);
    CHECK_FALSE(exc.ops.front().ticked);
    auto excludeBlank = oneOfFilter({"x"}, true);
    excludeBlank.exclude.push_back(blank);
    CHECK(ui::ticksOfFilter(excludeBlank).blanks == false);

    table::FilterEntry eq;
    eq.eq = "a";
    CHECK(ui::draftsOf(std::vector{eq, eq}).has_value());
    CHECK(ui::draftsOf(std::vector{eq, eq})->at(1).operand == "a");
    CHECK_FALSE(ui::draftsOf(std::vector{eq, eq, eq}).has_value());
    CHECK_FALSE(ui::draftsOf(std::vector{table::FilterEntry{}}).has_value());
    CHECK(ui::draftsOf({}).has_value());

    using ui::GroupMatch;
    CHECK(ui::columnCountOf(GroupMatch::EveryColumn, 5).kind == table::ColumnCount::Kind::All);
    CHECK(ui::columnCountOf(GroupMatch::AtLeast, 5).n == 5);
    CHECK(ui::columnCountOf(GroupMatch::AtLeast, 5).kind == table::ColumnCount::Kind::AtLeast);
    CHECK(ui::columnCountOf(GroupMatch::Exactly, 0).kind == table::ColumnCount::Kind::Exactly);
    CHECK(ui::columnCountOf(GroupMatch::Together, 5).kind == table::ColumnCount::Kind::Any);
    CHECK(ui::columnCountOf(GroupMatch::AnyColumn, 5).n == 1);
    CHECK(ui::matchLabel(GroupMatch::AnyColumn) == "in any column");
    CHECK(ui::matchLabel(GroupMatch::EveryColumn) == "in every column");
    CHECK(ui::matchLabel(GroupMatch::AtLeast) == "in at least N columns");
    CHECK(ui::matchLabel(GroupMatch::Exactly) == "in exactly N columns");
    CHECK(ui::matchLabel(GroupMatch::Together) == "all of them, each in a column");
    CHECK(ui::matchLabel(static_cast<GroupMatch>(99)).empty());
}

TEST_CASE("cov filter menu: instants and operands of date pickers", "[ui][table][filter_menu][cov]") {
    auto const day = fm::instantOf("2024-10-04");
    REQUIRE(day.has_value());
    CHECK(fm::operandOfInstant(**day, false) == "2024-10-04");
    auto const stamp = fm::instantOf("2024-10-04T08:30:15Z");
    REQUIRE(stamp.has_value());
    CHECK(fm::operandOfInstant(**stamp, true) == "2024-10-04T08:30:15Z");
    CHECK_FALSE(fm::instantOf("nonsense").has_value());
    // A time part needs 'T' at the tenth place and the whole 19 characters.
    auto const noTime = fm::instantOf("2024-10-04 08:30:15");
    REQUIRE(noTime.has_value());
    CHECK(fm::operandOfInstant(**noTime, true) == "2024-10-04T00:00:00Z");
}

TEST_CASE("cov filter menu: states differ by each field and a target finds its group",
          "[ui][table][filter_menu][cov]") {
    ui::FilterMenuState const base;
    CHECK(base == ui::FilterMenuState{});
    auto const differs = [&](auto change) {
        ui::FilterMenuState other;
        change(other);
        return !(base == other);
    };
    CHECK(differs([](ui::FilterMenuState& s) { s.open = true; }));
    CHECK(differs([](ui::FilterMenuState& s) { s.opening = 1; }));
    CHECK(differs([](ui::FilterMenuState& s) { s.search = "x"; }));
    CHECK(differs([](ui::FilterMenuState& s) { s.list.push_back({}); }));
    CHECK(differs([](ui::FilterMenuState& s) { s.distinct = 1; }));
    CHECK(differs([](ui::FilterMenuState& s) { s.total = 1; }));
    CHECK(differs([](ui::FilterMenuState& s) { s.blanks = 1; }));
    CHECK(differs([](ui::FilterMenuState& s) { s.invalid = 1; }));
    CHECK(differs([](ui::FilterMenuState& s) { s.partial = true; }));
    CHECK(differs([](ui::FilterMenuState& s) { s.listedOnce = true; }));
    CHECK(differs([](ui::FilterMenuState& s) { s.ticks.ticked = false; }));
    CHECK(differs([](ui::FilterMenuState& s) { s.ticks.blanks = true; }));
    CHECK(differs([](ui::FilterMenuState& s) { s.ticks.ops.emplace_back(); }));
    CHECK(differs([](ui::FilterMenuState& s) { s.dates.push_back("x"); }));
    CHECK(differs([](ui::FilterMenuState& s) { s.conditions.at(1).operand = "x"; }));
    CHECK(differs([](ui::FilterMenuState& s) { s.matchCase = true; }));
    CHECK(differs([](ui::FilterMenuState& s) { s.combine = table::Combine::All; }));
    CHECK(differs([](ui::FilterMenuState& s) { s.match = ui::GroupMatch::Exactly; }));
    CHECK(differs([](ui::FilterMenuState& s) { s.count = "2"; }));
    CHECK(differs([](ui::FilterMenuState& s) { s.refusal = "no"; }));
    CHECK(differs([](ui::FilterMenuState& s) { s.target.column = "c"; }));
    CHECK(differs([](ui::FilterMenuState& s) { s.target.group = "g"; }));
    CHECK(differs([](ui::FilterMenuState& s) { s.target.columns = {"a"}; }));
    auto left = base;
    auto right = base;
    left.ticks.ops.push_back({.search = "a", .operand = std::nullopt, .ticked = true});
    right.ticks.ops.push_back({.search = "b", .operand = std::nullopt, .ticked = true});
    CHECK_FALSE(left == right);
    right.ticks.ops.back().search = "a";
    CHECK(left == right);
    right.ticks.ops.back().operand = "x";
    CHECK_FALSE(left == right);
    right.ticks.ops.back().operand = std::nullopt;
    right.ticks.ops.back().ticked = false;
    CHECK_FALSE(left == right);

    table::FilterSpec spec;
    spec.groups.push_back({.group = "named"});
    spec.groups.push_back({.group = {}, .columns = {"a", "b"}});
    ui::FilterTarget named;
    named.group = "named";
    CHECK(ui::groupFilterIndex(spec, named) == 0);
    ui::FilterTarget formed;
    formed.formed = true;
    formed.columns = {"a", "b"};
    CHECK(ui::groupFilterIndex(spec, formed) == 1);
    formed.columns = {"a"};
    CHECK_FALSE(ui::groupFilterIndex(spec, formed).has_value());
    ui::FilterTarget const unnamed;
    CHECK_FALSE(ui::groupFilterIndex(spec, unnamed).has_value());
    named.group = "other";
    CHECK_FALSE(ui::groupFilterIndex(spec, named).has_value());
}

TEST_CASE("cov filter menu: opening lists the values and a tick then Apply writes the smaller side",
          "[ui][table][filter_menu][cov]") {
    Menu m;
    m.rows.answer = pagesOf(letters(4), 2);
    m.open("c");
    REQUIRE(m.state().open);
    CHECK(m.state().opening == 1);
    CHECK(m.state().listedOnce);
    CHECK(m.state().total == 4);
    CHECK(m.state().blanks == 2);
    CHECK(m.state().ticks.blanks == true);
    CHECK(m.labels() == std::vector<std::string>{"a (1)", "b (2)", "c (3)", "d (4)"});
    CHECK(m.menu.allTicked());
    CHECK(m.menu.canApply());
    CHECK(m.listings.back() == std::vector<std::string>{"c"});
    REQUIRE(m.rows.requests.size() == 1);
    CHECK(m.rows.requests.front().column == "c");
    CHECK(m.rows.requests.front().offset == 0);

    // Unticking "b" leaves one unticked of four: the filter names it, excluded.
    m.menu.tick(1, false);
    CHECK(m.touches > 0);
    CHECK_FALSE(m.menu.allTicked());
    CHECK_FALSE(m.menu.rows().at(1).checked);
    m.menu.apply();
    m.settle();
    REQUIRE(m.applied.size() == 1);
    auto const& written = m.applied.back().columns.at("c");
    REQUIRE(written.exclude.size() == 1);
    CHECK(written.exclude.front().oneOf == std::vector<std::string>{"b"});
    CHECK(written.include.empty());
    CHECK_FALSE(m.state().open);  // an applied filter closes the menu
    CHECK(m.listings.back().empty());

    // Opening again reads the stored filter back as ticks.
    m.open("c");
    CHECK_FALSE(m.menu.rows().at(1).checked);
    CHECK(m.menu.rows().at(0).checked);
    // Re-ticking everything removes the column's filter.
    m.menu.tickAll(true);
    m.menu.apply();
    m.settle();
    REQUIRE(m.applied.size() == 2);
    CHECK_FALSE(m.applied.back().columns.contains("c"));
}

TEST_CASE("cov filter menu: ticking none then some writes an include filter, ticking nothing writes none",
          "[ui][table][filter_menu][cov]") {
    Menu m;
    m.rows.answer = pagesOf(letters(4));
    m.open("c");
    m.menu.tickAll(false);
    CHECK_FALSE(m.menu.allTicked());
    CHECK_FALSE(m.menu.canApply());
    m.menu.apply();
    m.settle();
    CHECK(m.applied.empty());  // nothing ticked: nothing to apply
    m.menu.tick(2, true);
    CHECK(m.menu.canApply());
    m.menu.apply();
    m.settle();
    REQUIRE(m.applied.size() == 1);
    CHECK(m.applied.back().columns.at("c").include.front().oneOf == std::vector<std::string>{"c"});
    // A refused write keeps the menu open.
    Menu refused;
    refused.accept = false;
    refused.rows.answer = pagesOf(letters(4));
    refused.open("c");
    refused.menu.tickAll(false);
    refused.menu.tick(0, true);
    refused.menu.apply();
    refused.settle();
    CHECK(refused.state().open);
    CHECK(refused.applied.size() == 1);
}

TEST_CASE("cov filter menu: blanks are a box of their own", "[ui][table][filter_menu][cov]") {
    Menu m;
    m.rows.answer = pagesOf(letters(3), 1);
    m.open("c");
    m.menu.tickAll(false);
    CHECK(m.state().ticks.blanks == false);  // the select-all with no search reaches the blanks too
    m.menu.tickBlanks(true);
    CHECK(m.menu.canApply());
    m.menu.apply();
    m.settle();
    REQUIRE(m.applied.size() == 1);
    auto const& filter = m.applied.back().columns.at("c");
    REQUIRE(filter.include.size() == 1);
    CHECK(filter.include.front().isEmpty == true);
    // A column without empty cells has no blanks box.
    Menu none;
    none.rows.answer = pagesOf(letters(3), 0);
    none.open("c");
    CHECK_FALSE(none.state().ticks.blanks.has_value());
}

TEST_CASE("cov filter menu: a search narrows the list and select-all under it covers its values",
          "[ui][table][filter_menu][cov]") {
    Menu m;
    m.rows.answer = pagesOf({valueOf("apple", "apple"), valueOf("apricot", "apricot"), valueOf("berry", "berry")});
    m.open("c");
    CHECK(m.state().total == 3);
    m.menu.search("ap");
    m.settle();
    CHECK(m.state().search == "ap");
    CHECK(m.state().list.size() == 2);
    CHECK(m.state().total == 3);  // a search does not change the whole's size
    CHECK(m.rows.requests.back().search == "ap");
    CHECK(m.menu.canApply());
    m.menu.tickAll(false);  // unticks apple and apricot
    CHECK_FALSE(m.menu.rows().at(0).checked);
    m.menu.apply();
    m.settle();
    REQUIRE(m.applied.size() == 1);
    auto const& filter = m.applied.back().columns.at("c");
    // Two of three values unticked, both reached by the search: the filter names the one left, berry, which the
    // search did not reach and so was paged to be named.
    REQUIRE(filter.include.size() == 1);
    CHECK(filter.include.front().oneOf == std::vector<std::string>{"berry"});
}

TEST_CASE("cov filter menu: more pages in the next values, and a minority outside the list is paged to be named",
          "[ui][table][filter_menu][cov]") {
    Menu m;
    m.host.pageSize = 3;
    m.menu.setHost(m.host);
    m.rows.answer = pagesOf(letters(5));
    m.open("c");
    CHECK(m.state().list.size() == 3);
    m.menu.more();
    m.settle();
    CHECK(m.rows.requests.back().offset == 3);
    CHECK(m.state().list.size() == 5);
    // Untick all but "e": four unticked, one ticked, nothing outside the list.
    m.menu.tickAll(false);
    m.menu.tick(4, true);
    m.menu.apply();
    m.settle();
    REQUIRE(m.applied.size() == 1);
    CHECK(m.applied.back().columns.at("c").include.front().oneOf == std::vector<std::string>{"e"});

    // Four of five unticked in the list leaves the fifth, not in the first page, to be named by paging.
    Menu paged;
    paged.host.pageSize = 3;
    paged.menu.setHost(paged.host);
    paged.rows.answer = pagesOf(letters(5));
    paged.open("c");
    paged.menu.tick(0, false);
    paged.menu.tick(1, false);
    paged.menu.tick(2, false);
    paged.menu.apply();
    paged.settle();
    REQUIRE(paged.applied.size() == 1);
    auto const& include = paged.applied.back().columns.at("c").include;
    REQUIRE(include.size() == 1);
    CHECK(include.front().oneOf == std::vector<std::string>{"d", "e"});
}

TEST_CASE("cov filter menu: too many values to name is refused in the note", "[ui][table][filter_menu][cov]") {
    Menu m;
    m.host.maxSetValues = 1;
    m.menu.setHost(m.host);
    m.rows.answer = pagesOf(letters(6));
    m.open("c");
    m.menu.tick(0, false);
    m.menu.tick(1, false);
    m.menu.tick(2, false);
    m.menu.apply();
    m.settle();
    CHECK(m.applied.empty());
    CHECK(m.state().open);
    CHECK(m.menu.note() == ui::tableText::tooManyValues());
}

TEST_CASE("cov filter menu: rows that answer with an error leave the conditions alone",
          "[ui][table][filter_menu][cov]") {
    Menu m;
    m.rows.answer = [](table::DistinctRequest const&) -> DistinctReply {
        return std::unexpected(
            table::TableError{.code = table::TableErrorCode::Unavailable, .column = {}, .message = "no list"});
    };
    m.open("c");
    CHECK_FALSE(m.state().target.listed);
    CHECK(m.state().list.empty());
    CHECK(m.state().listedOnce);
    // Without rows the answer is the same.
    Menu bare;
    bare.host.rows = [] { return static_cast<ui::TableRows*>(nullptr); };
    bare.menu.setHost(bare.host);
    bare.open("c");
    CHECK_FALSE(bare.state().target.listed);
    // A failing page while naming the values is the note.
    Menu failing;
    failing.rows.answer = pagesOf(letters(3));
    failing.open("c");
    failing.menu.search("a");
    failing.settle();
    failing.menu.tickAll(false);
    failing.rows.answer = [](table::DistinctRequest const&) -> DistinctReply {
        return std::unexpected(
            table::TableError{.code = table::TableErrorCode::InvalidSpec, .column = {}, .message = "boom"});
    };
    failing.menu.apply();
    failing.settle();
    CHECK(failing.menu.note() == "boom");
    CHECK(failing.applied.empty());
}

TEST_CASE("cov filter menu: conditions are typed, validated and sent after the debounce",
          "[ui][table][filter_menu][cov]") {
    using ui::ConditionOp;
    Menu m;
    m.rows.answer = pagesOf(letters(3));
    m.open("n", ColumnKind::Integer);
    m.menu.setOp(0, ConditionOp::Greater);
    m.menu.setOperand(0, "5");
    m.settle();
    REQUIRE(m.applied.size() == 1);  // no scheduler: sent at once
    CHECK(m.applied.back().columns.at("n").include.front().gt == "5");
    // An operand that is not a number is flagged and nothing is sent.
    m.menu.setOperand(0, "abc");
    CHECK(m.state().conditions.at(0).invalid);
    CHECK(m.applied.size() == 1);
    // An empty operand is incomplete, not invalid.
    m.menu.setOperand(0, "");
    CHECK_FALSE(m.state().conditions.at(0).invalid);
    CHECK(m.applied.size() == 1);
    // Between needs both ends.
    m.menu.setOp(0, ConditionOp::Between);
    m.menu.setOperand(0, "1");
    CHECK(m.applied.size() == 1);
    m.menu.setSecond(0, "x");
    CHECK(m.state().conditions.at(0).invalid);
    m.menu.setSecond(0, "9");
    REQUIRE(m.applied.size() == 2);
    CHECK(m.applied.back().columns.at("n").include.front().between == std::array<std::string, 2>{"1", "9"});
    // Two conditions joined by "or".
    m.menu.setCombine(table::Combine::All);
    m.menu.setOp(1, ConditionOp::NotEquals);
    m.menu.setOperand(1, "4");
    CHECK(m.applied.back().columns.at("n").combine == table::Combine::All);
    CHECK(m.applied.back().columns.at("n").include.size() == 2);
    // Apply with conditions sends them and closes.
    m.menu.apply();
    CHECK_FALSE(m.state().open);
    // A filter that is already held is not applied again.
    std::size_t const sent = m.applied.size();
    m.open("n", ColumnKind::Integer);
    CHECK(m.state().conditions.at(0).op == ConditionOp::Between);
    CHECK(m.state().combine == table::Combine::All);
    m.menu.setOperand(0, "1");
    CHECK(m.applied.size() == sent);
    // Removing the conditions removes the filter.
    m.menu.setOp(0, ConditionOp::IsEmpty);
    m.menu.setOp(1, ConditionOp::IsNotEmpty);
    CHECK(m.applied.back().columns.at("n").include.front().isEmpty == true);
}

TEST_CASE("cov filter menu: a text condition can match case and clearing removes the filter",
          "[ui][table][filter_menu][cov]") {
    using ui::ConditionOp;
    Menu m;
    m.rows.answer = pagesOf(letters(3));
    m.open("t");
    m.menu.setOp(0, ConditionOp::Contains);
    m.menu.setOperand(0, "A");
    m.menu.setMatchCase(true);
    CHECK(m.applied.back().columns.at("t").include.front().caseSensitive == true);
    m.menu.setMatchCase(false);
    CHECK_FALSE(m.applied.back().columns.at("t").include.front().caseSensitive.has_value());
    m.menu.clearFilter();
    CHECK_FALSE(m.applied.back().columns.contains("t"));
    CHECK_FALSE(m.state().conditions.at(0).op.has_value());
    CHECK(m.state().ticks.ticked);
    // An open menu's stored case-sensitive condition reads back.
    table::FilterEntry entry;
    entry.contains = "x";
    entry.caseSensitive = true;
    m.spec.columns["t"] = table::ColumnFilter{.combine = table::Combine::Any, .include = {entry}, .exclude = {}};
    m.open("t");
    CHECK(m.state().matchCase);
    CHECK(m.state().conditions.at(0).operand == "x");
    // Closing twice is harmless.
    m.menu.close();
    m.menu.close();
    CHECK_FALSE(m.state().open);
}

TEST_CASE("cov filter menu: a date column lists years, opens a year into months and applies ranges",
          "[ui][table][filter_menu][cov]") {
    Menu m;
    m.rows.answer = [](table::DistinctRequest const& request) -> DistinctReply {
        table::DistinctValues out;
        if (request.bucket == table::DateBucket::Year) {
            out.values = {valueOf("2024", "2024-01-01..2024-12-31", 3), valueOf("2025", "2025-01-01..2025-12-31", 1)};
        } else if (request.bucket == table::DateBucket::Month) {
            out.values = {valueOf("2024-01", "2024-01-01..2024-01-31", 2),
                          valueOf("2024-02", "2024-02-01..2024-02-29", 1)};
        }
        out.distinct = static_cast<std::int64_t>(out.values.size());
        out.blanks = 1;
        return out;
    };
    m.open("d", ColumnKind::Date);
    CHECK_FALSE(m.state().ticks.ticked);
    CHECK(m.state().ticks.blanks == false);
    REQUIRE(m.menu.rows().size() == 2);
    CHECK(m.menu.rows().front().opens);
    CHECK(m.menu.canApply());
    m.menu.openBucket(0);
    m.settle();
    REQUIRE(m.state().list.size() == 4);
    CHECK(m.state().list.at(1).depth == 1);
    CHECK(m.rows.requests.back().search == "2024-");
    // Ticking the year ticks the months under it; they cannot be toggled alone.
    m.menu.tick(0, true);
    auto const rows = m.menu.rows();
    CHECK(rows.at(1).checked);
    CHECK_FALSE(rows.at(1).enabled);
    // Unticking one month keeps the rest of the year.
    m.menu.tick(0, false);
    m.menu.tick(1, true);
    m.menu.tick(1, false);
    CHECK(m.state().dates.empty());
    m.menu.tick(0, true);
    m.menu.tick(2, false);
    CHECK(m.state().dates == std::vector<std::string>{"2024-01-01..2024-01-31", "2024-03-01..2024-12-31"});
    // Collapse, then apply the ranges.
    m.menu.openBucket(0);
    CHECK(m.state().list.size() == 2);
    m.menu.openBucket(1);  // opening a year not yet opened is asked again, and a day bucket would not open
    m.menu.openBucket(99);
    m.menu.tickBlanks(true);
    m.menu.apply();
    m.settle();
    REQUIRE(m.applied.size() == 1);
    auto const& filter = m.applied.back().columns.at("d");
    CHECK(filter.include.size() == 3);  // two ranges and the blanks
    CHECK(filter.include.back().isEmpty == true);
}

TEST_CASE("cov filter menu: a date filter refuses too many ranges and clears with nothing ticked",
          "[ui][table][filter_menu][cov]") {
    Menu m;
    m.host.maxEntries = 1;
    m.menu.setHost(m.host);
    m.rows.answer = [](table::DistinctRequest const&) -> DistinctReply {
        table::DistinctValues out;
        out.values = {valueOf("2024", "2024-01-01..2024-12-31"), valueOf("2026", "2026-01-01..2026-12-31")};
        out.distinct = 2;
        return out;
    };
    m.spec.columns["d"] = table::ColumnFilter{};
    m.open("d", ColumnKind::DateTime);
    m.menu.apply();  // nothing ticked: the column's filter goes
    m.settle();
    REQUIRE(m.applied.size() == 1);
    CHECK_FALSE(m.applied.back().columns.contains("d"));
    m.open("d", ColumnKind::DateTime);
    m.menu.tick(0, true);
    m.menu.tick(1, true);
    m.menu.apply();
    CHECK(m.state().refusal == ui::tableText::tooManyDates());
    CHECK(m.applied.size() == 1);
}

TEST_CASE("cov filter menu: a stored date filter opens with its ranges ticked", "[ui][table][filter_menu][cov]") {
    Menu m;
    m.rows.answer = [](table::DistinctRequest const&) -> DistinctReply {
        table::DistinctValues out;
        out.values = {valueOf("2024", "2024-01-01..2024-12-31")};
        out.distinct = 1;
        return out;
    };
    table::FilterEntry range;
    range.between = std::array<std::string, 2>{"2024-01-01", "2024-12-31"};
    table::FilterEntry blank;
    blank.isEmpty = true;
    m.spec.columns["d"] =
        table::ColumnFilter{.combine = table::Combine::Any, .include = {range, blank}, .exclude = {}};
    m.open("d", ColumnKind::Date);
    CHECK(m.state().dates == std::vector<std::string>{"2024-01-01..2024-12-31"});
    CHECK(m.state().ticks.blanks == true);
    CHECK(m.menu.rows().front().checked);
}

TEST_CASE("cov filter menu: a group's values filter is written with its match", "[ui][table][filter_menu][cov]") {
    using ui::GroupMatch;
    Menu m;
    m.rows.answer = pagesOf(letters(3));
    m.openGroup("g1", {"a", "b", "c"});
    CHECK_FALSE(m.state().ticks.ticked);
    CHECK(m.listings.back() == std::vector<std::string>{"a", "b", "c"});
    CHECK(m.rows.requests.front().group == "g1");
    m.menu.tick(2, true);
    m.menu.tick(0, true);
    m.menu.setMatch(GroupMatch::AtLeast);
    m.menu.setCount("1");
    m.menu.apply();
    m.settle();
    REQUIRE(m.applied.size() == 1);
    auto const& filter = m.applied.back().groups.front();
    CHECK(filter.group == "g1");
    CHECK(filter.match.kind == table::ColumnCount::Kind::AtLeast);
    CHECK(filter.include.front().oneOf == std::vector<std::string>{"a", "c"});  // in the list's order
    // The stored group filter reads back; together needs two ticked values.
    m.open("x");
    m.menu.close();
    m.openGroup("g1", {"a", "b", "c"});
    CHECK(m.state().match == GroupMatch::AtLeast);
    CHECK(m.state().count == "1");
    CHECK(m.menu.rows().at(0).checked);
    m.menu.setMatch(GroupMatch::Together);
    m.menu.tick(0, false);  // below two ticked values together falls back to any column
    CHECK(m.state().match == GroupMatch::AnyColumn);
    m.menu.tick(0, true);
    m.menu.tick(1, true);
    m.menu.setMatch(GroupMatch::Together);
    m.menu.apply();
    m.settle();
    CHECK(m.applied.back().groups.front().mode == table::GroupMode::Together);
    // Together values read back too.
    m.openGroup("g1", {"a", "b", "c"});
    CHECK(m.state().match == GroupMatch::Together);
    // A bad N is the note.
    m.menu.setMatch(GroupMatch::Exactly);
    m.menu.setCount("9");
    m.menu.apply();
    m.settle();
    CHECK(m.menu.note() == "N is 0 to 3");
    // Clearing a group's filter removes it.
    m.menu.clearFilter();
    CHECK(m.applied.back().groups.empty());
    // A formed group has no values list to apply.
    m.openGroup("", {"a", "b"}, true);
    std::size_t const sent = m.applied.size();
    m.menu.apply();
    CHECK(m.applied.size() == sent);
    CHECK_FALSE(m.menu.canApply());
}

TEST_CASE("cov filter menu: group conditions are written to a group filter and read back",
          "[ui][table][filter_menu][cov]") {
    using ui::ConditionOp;
    using ui::GroupMatch;
    Menu m;
    m.rows.answer = pagesOf(letters(2));
    m.openGroup("", {"a", "b"}, true);
    m.menu.setOp(0, ConditionOp::Contains);
    m.menu.setOperand(0, "z");
    m.menu.setMatch(GroupMatch::Exactly);
    m.menu.setCount("1");
    REQUIRE_FALSE(m.applied.empty());
    auto const& filter = m.applied.back().groups.front();
    CHECK(filter.columns == std::vector<std::string>{"a", "b"});
    CHECK(filter.group.empty());
    CHECK(filter.match.kind == table::ColumnCount::Kind::Exactly);
    // Changing the condition replaces the group's filter in place.
    m.menu.setOperand(0, "y");
    CHECK(m.applied.back().groups.size() == 1);
    m.openGroup("", {"a", "b"}, true);
    CHECK(m.state().conditions.at(0).operand == "y");
    CHECK(m.state().match == GroupMatch::Exactly);
    // No condition left removes the group's filter.
    m.menu.setOp(0, ConditionOp::IsEmpty);
    CHECK(m.applied.back().groups.front().include.front().isEmpty == true);
}

namespace {

// A menu whose host reaches the rows and nothing else: it has no filters to read, nowhere to apply, no touch hook, no
// listing hook and no scheduler.
struct BareMenu {
    morph::testing::StepExecutor owner;
    morph::reactive::Runtime runtime{owner};
    ScriptedRows rows{owner};
    ui::FilterMenu menu{runtime};
    BareMenu() {
        ui::FilterMenuHost host;
        host.rows = [this] { return &rows; };
        host.format = morph::ui::testing::formatFor("en");
        menu.setHost(host);
    }
    void open(const std::string& column, ColumnKind kind = ColumnKind::Text) {
        ui::FilterTarget target;
        target.column = column;
        target.shown = table::TableColumn{.info = {.id = column, .kind = kind}, .label = column};
        menu.open(std::move(target));
        owner.runAll();
    }
};
}  // namespace

TEST_CASE("cov filter menu: a menu that is not open lists nothing and a host without hooks is not needed",
          "[ui][table][filter_menu][cov]") {
    BareMenu m;
    CHECK(m.menu.rows().empty());
    CHECK_FALSE(m.menu.allTicked());
    m.menu.close();  // nothing to close
    m.rows.answer = pagesOf(letters(3), 1);
    m.open("c");
    CHECK(m.menu.rows().size() == 3);
    m.menu.tick(0, false);
    m.menu.tick(99, true);  // past the list: ignored
    m.menu.apply();
    m.owner.runAll();
    CHECK(m.menu.state().open);  // nowhere to apply: the menu stays
    m.menu.clearFilter();
    CHECK(m.menu.state().ticks.ticked);
    m.menu.setOp(0, ui::ConditionOp::Contains);
    m.menu.setOperand(0, "a");
    CHECK(m.menu.state().open);
    m.menu.apply();  // conditions that cannot be applied leave the menu open
    CHECK(m.menu.state().open);
}

TEST_CASE("cov filter menu: a reply for a menu that closed or reopened is not shown",
          "[ui][table][filter_menu][cov]") {
    Menu m;
    m.rows.answer = pagesOf(letters(3));
    m.open("c");
    m.menu.search("a");
    m.settle();
    m.menu.tickAll(false);
    m.menu.apply();  // names the values the search reaches: a question to the rows
    m.menu.close();
    m.settle();
    CHECK(m.applied.empty());  // the answer came to a menu that is no longer the one that asked
    // A search whose reply arrives after the menu closes changes nothing.
    m.open("c");
    m.menu.search("a");
    m.menu.close();
    m.settle();
    CHECK(m.state().list.empty());
}

TEST_CASE("cov filter menu: a failed list that is not unavailable keeps the list on offer",
          "[ui][table][filter_menu][cov]") {
    Menu m;
    m.rows.answer = [](table::DistinctRequest const&) -> DistinctReply {
        return std::unexpected(
            table::TableError{.code = table::TableErrorCode::InvalidSpec, .column = {}, .message = "bad"});
    };
    m.open("c");
    CHECK(m.state().target.listed);
    CHECK(m.state().list.empty());
    CHECK(m.state().listedOnce);
}

TEST_CASE("cov filter menu: a group's ticked values are paged to be named and bounded",
          "[ui][table][filter_menu][cov]") {
    Menu m;
    m.rows.answer = pagesOf(letters(4));
    m.openGroup("g1", {"a", "b", "c", "d"});
    m.menu.tickAll(true);
    m.menu.tick(1, false);
    m.menu.apply();
    m.settle();
    REQUIRE(m.applied.size() == 1);
    CHECK(m.applied.back().groups.front().include.front().oneOf == std::vector<std::string>{"a", "c", "d"});
    // Too many to name.
    Menu bounded;
    bounded.host.maxSetValues = 2;
    bounded.menu.setHost(bounded.host);
    bounded.rows.answer = pagesOf(letters(4));
    bounded.openGroup("g1", {"a", "b", "c", "d"});
    bounded.menu.tickAll(true);
    bounded.menu.apply();
    bounded.settle();
    CHECK(bounded.applied.empty());
    CHECK(bounded.menu.note() == ui::tableText::tooManyValues());
    // Values that cannot be paged are the note.
    Menu failing;
    failing.rows.answer = pagesOf(letters(4));
    failing.openGroup("g1", {"a", "b"});
    failing.menu.tickAll(true);
    failing.rows.answer = [](table::DistinctRequest const&) -> DistinctReply {
        return std::unexpected(
            table::TableError{.code = table::TableErrorCode::InvalidSpec, .column = {}, .message = "gone"});
    };
    failing.menu.apply();
    failing.settle();
    CHECK(failing.menu.note() == "gone");
}

TEST_CASE("cov filter menu: years open into months and days, collapse together, and a day does not open",
          "[ui][table][filter_menu][cov]") {
    Menu m;
    m.rows.answer = [](table::DistinctRequest const& request) -> DistinctReply {
        table::DistinctValues out;
        if (request.bucket == table::DateBucket::Year) {
            out.values = {valueOf("2024", "2024-01-01..2024-12-31", 3)};
        } else if (request.bucket == table::DateBucket::Month) {
            out.values = {valueOf("2024-01", "2024-01-01..2024-01-31", 2)};
        } else {
            out.values = {valueOf("2024-01-05", "2024-01-05..2024-01-05", 1)};
        }
        out.distinct = static_cast<std::int64_t>(out.values.size());
        return out;
    };
    m.open("d", ColumnKind::Date);
    m.menu.openBucket(0);  // the year
    m.settle();
    m.menu.openBucket(1);  // its month
    m.settle();
    REQUIRE(m.state().list.size() == 3);
    CHECK(m.state().list.at(2).depth == 2);
    m.menu.openBucket(2);  // a day: nothing to open
    m.settle();
    CHECK(m.state().list.size() == 3);
    m.menu.openBucket(0);  // collapsing the year takes its months and their days
    CHECK(m.state().list.size() == 1);
    m.menu.openBucket(0);
    m.settle();
    CHECK(m.state().list.size() == 2);
    m.menu.close();
    m.menu.openBucket(0);  // closed: the list is empty
    CHECK(m.state().list.empty());
}

TEST_CASE("cov filter menu: a measure condition starts in the first unit and ticked blanks alone are a date filter",
          "[ui][table][filter_menu][cov]") {
    Menu m;
    m.rows.answer = pagesOf({}, 2);
    table::FilterEntry gt;
    gt.gt = "5 kg";
    m.spec.columns["w"] = table::ColumnFilter{.combine = table::Combine::Any, .include = {gt}, .exclude = {}};
    ui::FilterTarget target;
    target.column = "w";
    target.shown = table::TableColumn{.info = {.id = "w", .kind = ColumnKind::Measure}};
    target.unitChoices = {"kg", "g"};
    target.unitList = {table::UnitInfo{
        .id = "kg", .symbol = "kg", .dimension = "mass", .toBase = *table::parseDecimal("1"), .decimals = 3}};
    m.menu.open(std::move(target));
    m.settle();
    CHECK(m.state().conditions.at(0).op == ui::ConditionOp::Greater);
    CHECK(m.state().conditions.at(0).unit == "kg");
    CHECK(m.state().conditions.at(1).unit == "kg");

    Menu blanks;
    blanks.rows.answer = pagesOf({}, 2);
    blanks.open("d", ColumnKind::Date);
    blanks.menu.tickBlanks(true);
    blanks.menu.apply();
    blanks.settle();
    REQUIRE(blanks.applied.size() == 1);
    CHECK(blanks.applied.back().columns.at("d").include.front().isEmpty == true);
}

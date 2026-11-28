// SPDX-License-Identifier: Apache-2.0

// The filter menu's group filters read back for every quantifier, a condition's debounce of nothing, and the
// choices of a match line the menu has no name for.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <morph/core/completion.hpp>
#include <morph/reactive/testing/manual_scheduler.hpp>
#include <morph/table/data_source.hpp>
#include <morph/table/filter.hpp>
#include <morph/table/query.hpp>
#include <morph/testing/step_executor.hpp>
#include <morph/ui/table_filter_menu.hpp>
#include <morph/ui/table_rows.hpp>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ui_table_support.hpp"

namespace {

namespace ui = morph::ui;
namespace table = morph::table;
using table::ColumnKind;
using DistinctReply = std::expected<table::DistinctValues, table::TableError>;

table::DistinctValue valueOf(std::string text, std::string operand, std::int64_t count = 1) {
    return table::DistinctValue{.text = std::move(text), .operand = std::move(operand), .count = count};
}

// Rows that list no cells and answer a values list from a function.
class ScriptedRows final : public ui::TableRows {
public:
    explicit ScriptedRows(morph::exec::IExecutor& owner) : _owner{&owner} {}

    std::function<DistinctReply(table::DistinctRequest const&)> answer;
    std::vector<table::DistinctRequest> requests;

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
        for (std::size_t at = static_cast<std::size_t>(request.offset);
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
    morph::reactive::testing::ManualScheduler scheduler;
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

    void open(std::string column, ColumnKind kind = ColumnKind::Text) {
        ui::FilterTarget target;
        target.column = column;
        target.label = column;
        target.shown = table::TableColumn{.info = {.id = column, .kind = kind}, .label = column};
        menu.open(std::move(target));
        settle();
    }
    void openGroup(std::string group, std::vector<std::string> columns, bool formed = false) {
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

TEST_CASE("covj filter menu: a stored group filter reads back with its quantifier", "[covj][ui][table][filter_menu]") {
    struct Case {
        table::ColumnCount::Kind kind;
        std::int64_t n;
        ui::GroupMatch match;
        std::string count;
    };
    auto const [kind, n, match, count] =
        GENERATE(Case{table::ColumnCount::Kind::Any, 1, ui::GroupMatch::AnyColumn, "1"},
                 Case{table::ColumnCount::Kind::All, 1, ui::GroupMatch::EveryColumn, "1"},
                 Case{table::ColumnCount::Kind::AtLeast, 2, ui::GroupMatch::AtLeast, "2"},
                 Case{table::ColumnCount::Kind::Exactly, 3, ui::GroupMatch::Exactly, "3"},
                 Case{static_cast<table::ColumnCount::Kind>(99), 1, ui::GroupMatch::AnyColumn, "1"});
    Menu m;
    m.rows.answer = pagesOf(letters(3));
    table::FilterEntry entry;
    entry.oneOf = std::vector<std::string>{"a"};
    m.spec.groups.push_back(table::GroupFilter{.group = "g1",
                                               .columns = {},
                                               .mode = table::GroupMode::Columns,
                                               .match = {.kind = kind, .n = n},
                                               .combine = table::Combine::Any,
                                               .include = {entry},
                                               .exclude = {}});
    m.openGroup("g1", {"a", "b", "c"});
    CHECK(m.state().match == match);
    CHECK(m.state().count == count);
    REQUIRE_FALSE(m.menu.rows().empty());
    CHECK(m.menu.rows().front().checked);
}

TEST_CASE("covj filter menu: a stored together filter ticks its values and blanks", "[covj][ui][table][filter_menu]") {
    Menu m;
    m.rows.answer = pagesOf(letters(3));
    table::FilterEntry first;
    first.eq = "a";
    table::FilterEntry blank;
    blank.isEmpty = true;
    m.spec.groups.push_back(table::GroupFilter{.group = "g1",
                                               .columns = {},
                                               .mode = table::GroupMode::Together,
                                               .match = {},
                                               .combine = table::Combine::Any,
                                               .include = {first},
                                               .exclude = {}});
    m.openGroup("g1", {"a", "b", "c"});
    CHECK(m.state().match == ui::GroupMatch::Together);
    REQUIRE_FALSE(m.menu.rows().empty());
    CHECK(m.menu.rows().front().checked);
    SECTION("a blank entry beside the values is ticked blanks") {
        m.spec.groups.front().include.push_back(blank);
        m.spec.groups.front().include.front().oneOf = std::vector<std::string>{"b"};
        m.spec.groups.front().include.front().eq = std::nullopt;
        m.spec.groups.front().mode = table::GroupMode::Columns;
        m.rows.answer = pagesOf(letters(3), 2);
        m.openGroup("g1", {"a", "b", "c"});
        CHECK(m.state().ticks.blanks == true);
    }
}

TEST_CASE("covj filter menu: a match no choice names is a column quantifier of any and has no label",
          "[covj][ui][table][filter_menu]") {
    auto const unknown = static_cast<ui::GroupMatch>(99);
    CHECK(ui::columnCountOf(unknown, 4).kind == table::ColumnCount::Kind::Any);
    CHECK(ui::columnCountOf(unknown, 4).n == 1);
    CHECK(ui::matchLabel(unknown).empty());
    CHECK(ui::columnCountOf(ui::GroupMatch::Together, 4).kind == table::ColumnCount::Kind::Any);
    CHECK(ui::columnCountOf(ui::GroupMatch::AtLeast, 4).n == 4);
}

TEST_CASE("covj filter menu: a debounce of nothing applies a condition at once", "[covj][ui][table][filter_menu]") {
    Menu m;
    m.rows.answer = pagesOf(letters(3));
    m.host.scheduler = &m.scheduler;
    m.menu.setHost(m.host);
    m.open("x");
    m.menu.setOp(0, ui::ConditionOp::Contains);
    SECTION("a debounce of nothing") {
        m.host.debounce = std::chrono::milliseconds{0};
        m.menu.setHost(m.host);
        m.menu.setOperand(0, "ab");
        m.settle();
        REQUIRE(m.applied.size() == 1);
    }
    SECTION("a debounce that waits") {
        m.menu.setOperand(0, "ab");
        m.settle();
        CHECK(m.applied.empty());
        m.scheduler.advance(std::chrono::milliseconds{250});
        m.settle();
        REQUIRE(m.applied.size() == 1);
    }
}

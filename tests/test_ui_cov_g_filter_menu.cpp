// SPDX-License-Identifier: Apache-2.0

// The filter menu's reads of a calendar day, and its life around a values list that is asked and answered later: a
// reply to a menu that closed or reopened since is dropped, a closed menu asks nothing, and rows that list nothing
// leave the conditions alone.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <morph/core/completion.hpp>
#include <morph/reactive/runtime.hpp>
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
namespace fm = morph::ui::detail::filtermenu;
using DistinctReply = std::expected<table::DistinctValues, table::TableError>;
using Pending = morph::async::Completion<DistinctReply>::Promise;

// Rows that hold each values request open until the case answers it.
class HeldRows final : public ui::TableRows {
public:
    explicit HeldRows(morph::exec::IExecutor& owner) : _owner{&owner} {}

    std::vector<table::DistinctRequest> requests;
    std::vector<Pending> held;

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
        requests.push_back(std::move(request));
        auto [completion, promise] = morph::async::Completion<DistinctReply>::makeSettleable(_owner);
        held.push_back(std::move(promise));
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

DistinctReply listing(std::vector<std::string> const& names) {
    table::DistinctValues out;
    for (std::string const& name : names) {
        out.values.push_back(table::DistinctValue{.text = name, .operand = name, .count = 1});
    }
    out.distinct = static_cast<std::int64_t>(names.size());
    return out;
}

struct Rig {
    morph::testing::StepExecutor owner;
    morph::reactive::Runtime runtime{owner};
    HeldRows rows{owner};
    table::FilterSpec spec;
    std::vector<table::FilterSpec> applied;
    ui::FilterMenu menu{runtime};
    ui::FilterMenuHost host;
    bool withRows = true;

    explicit Rig(bool listsValues = true) : withRows{listsValues} {
        host.rows = [this]() -> ui::TableRows* { return withRows ? &rows : nullptr; };
        host.filters = [this] { return spec; };
        host.apply = [this](table::FilterSpec next) {
            applied.push_back(next);
            spec = std::move(next);
            return true;
        };
        host.touch = [] {};
        host.format = morph::ui::testing::formatFor("en");
        menu.setHost(host);
    }
    void settle() { owner.runAll(); }
    void open(std::string const& column) {
        ui::FilterTarget target;
        target.column = column;
        target.label = column;
        target.shown = table::TableColumn{.info = {.id = column, .kind = table::ColumnKind::Text}, .label = column};
        menu.open(std::move(target));
        settle();
    }
    void answer(std::size_t index, DistinctReply reply) {
        held.at(index).resolve(std::move(reply));
        settle();
    }
    std::vector<Pending>& held = rows.held;
};

}  // namespace

TEST_CASE("covg filter menu: a day is read only when its text is a calendar day", "[covg][ui][table][filter_menu]") {
    CHECK(fm::dayOf("2024-10x04") == std::nullopt);  // no hyphen where the month ends
    CHECK(fm::dayOf("2024-xx-04") == std::nullopt);
    CHECK(fm::dayOf("2024-10-04").has_value());
    // A bucket is covered by a held range only when the held text is a range itself.
    auto const covered = fm::coverOf(std::vector<std::string>{"junk"}, "2024-01-01..2024-01-31");
    CHECK_FALSE(covered.first);
    CHECK_FALSE(covered.second);
}

TEST_CASE("covg filter menu: a values list that arrives after the menu closed is dropped",
          "[covg][ui][table][filter_menu]") {
    Rig rig;
    rig.open("name");
    REQUIRE(rig.rows.requests.size() == 1);
    rig.menu.close();
    rig.answer(0, listing({"a", "b"}));
    CHECK_FALSE(rig.menu.state().open);
    CHECK(rig.menu.state().list.empty());
}

TEST_CASE("covg filter menu: a reply to an earlier opening does not fill the later one",
          "[covg][ui][table][filter_menu]") {
    Rig rig;
    rig.open("name");
    rig.menu.close();
    rig.open("name");
    REQUIRE(rig.rows.requests.size() == 2);
    rig.answer(0, listing({"stale"}));
    CHECK(rig.menu.state().list.empty());
    rig.answer(1, listing({"fresh", "other"}));
    REQUIRE(rig.menu.state().list.size() == 2);
    CHECK(rig.menu.state().list.front().value.text == "fresh");
}

TEST_CASE("covg filter menu: a menu that is not open asks for nothing", "[covg][ui][table][filter_menu]") {
    Rig rig;
    rig.menu.more();
    rig.settle();
    CHECK(rig.rows.requests.empty());
}

TEST_CASE("covg filter menu: rows that cannot be asked leave the conditions alone", "[covg][ui][table][filter_menu]") {
    Rig rig{false};
    rig.open("name");
    CHECK(rig.rows.requests.empty());
    CHECK_FALSE(rig.menu.state().target.listed);
    CHECK(rig.menu.state().open);
}

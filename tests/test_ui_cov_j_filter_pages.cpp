// SPDX-License-Identifier: Apache-2.0

// The filter menu's apply over a values list that is only partly loaded: the rest is paged to be named, and a failed
// or late page is refused or dropped without a filter being written.

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

namespace {

// Opens the menu on a list of a, b and c, narrows it to "b" and unticks what the search shows, then applies: the
// menu asks for every value the search matches, and that request is the last one held.
void applyUnderSearch(Rig& rig) {
    rig.open("name");
    rig.answer(0, listing({"a", "b", "c"}));
    rig.menu.search("b");
    rig.settle();
    rig.answer(1, listing({"b"}));
    rig.menu.tickAll(false);
    rig.menu.apply();
    rig.settle();
}

}  // namespace

TEST_CASE("covj filter menu: applying under a search pages what it matches, and a failed page is refused",
          "[covj][ui][table][filter_menu]") {
    Rig rig;
    applyUnderSearch(rig);
    REQUIRE(rig.held.size() == 3);
    rig.held.back().resolve(std::unexpected(
        table::TableError{.code = table::TableErrorCode::ServiceFailed, .column = {}, .message = "gone"}));
    rig.settle();
    CHECK(rig.applied.empty());
    CHECK(rig.menu.state().open);
    CHECK(rig.menu.note().contains("gone"));
}

TEST_CASE("covj filter menu: the pages of an apply that the menu closed on are dropped",
          "[covj][ui][table][filter_menu]") {
    Rig rig;
    applyUnderSearch(rig);
    REQUIRE(rig.held.size() == 3);
    rig.menu.close();
    rig.held.back().resolve(listing({"b"}));
    rig.settle();
    CHECK(rig.applied.empty());
    CHECK_FALSE(rig.menu.state().open);
}

TEST_CASE("covj filter menu: the pages of an apply that finish while the menu is open write the filter",
          "[covj][ui][table][filter_menu]") {
    Rig rig;
    applyUnderSearch(rig);
    REQUIRE(rig.held.size() == 3);
    rig.held.back().resolve(listing({"b"}));
    rig.settle();
    REQUIRE(rig.applied.size() == 1);
    CHECK(rig.applied.back().columns.contains("name"));
}

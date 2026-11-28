// SPDX-License-Identifier: Apache-2.0

// `EngineRows` over a document's rows: counting a view selection, reading a range, finding a row, and the retained
// rows' bookkeeping when a row's key changes.

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <morph/table/engine.hpp>
#include <morph/table/filter.hpp>
#include <morph/ui/table_rows.hpp>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "ui_table_model_support.hpp"
#include "ui_table_support.hpp"

namespace {

namespace table = morph::table;
namespace ui = morph::ui;
using namespace table_model_support;
using morph::ui::EngineRows;
using morph::ui::ValueList;
using morph::ui::testing::RowsFixture;

table::RowId id(std::int64_t key) { return table::RowId{key}; }

ValueList numbered(std::size_t n) {
    ValueList rows;
    for (std::size_t at = 0; at < n; ++at) {
        auto const i = static_cast<std::int64_t>(at);
        rows.push_back(rec({{"id", i}, {"name", "n" + std::to_string(at)}}));
    }
    return rows;
}

struct Fixture {
    RowsFixture h;
    std::shared_ptr<ui::ValueRowsSource> source = sourceOf(h, {col("name")});
    std::unique_ptr<EngineRows> rows;
    explicit Fixture(std::size_t n = 10) {
        source->use("name");
        source->assign(numbered(n));
        h.settle();
        rows = std::make_unique<EngineRows>(source, ui::ValueRowsSource::recordOf(),
                                            table::EngineOptions{.owner = &h.owner});
        h.settle();
    }
    void filterTo(std::string name) {
        table::FilterEntry entry;
        entry.eq = std::move(name);
        table::FilterSpec spec;
        spec.columns["name"] = table::ColumnFilter{.combine = table::Combine::Any, .include = {entry}, .exclude = {}};
        auto const applied = rows->setFilter(std::move(spec));
        REQUIRE(applied);
        h.settle();
    }
};
}  // namespace

TEST_CASE("cov engine rows: a view selection counts the view less the rows it excepts", "[ui][table][cov]") {
    Fixture f;
    ui::TableSelection keys;
    keys.keys = {id(1), id(2)};
    CHECK(f.rows->selectedCount(keys).rows == 2);
    auto const view = f.rows->selectView();
    REQUIRE(view.has_value());
    ui::TableSelection whole;
    whole.view = *view;
    CHECK(f.rows->selectedCount(whole).rows == 10);
    whole.except = {id(3), id(4), id(99)};  // 99 is not in the view and takes nothing off
    CHECK(f.rows->selectedCount(whole).rows == 8);
    CHECK(f.rows->selectedCount(whole).kind == ui::CountKind::Exact);
    CHECK(view->retained.empty());
}

TEST_CASE("cov engine rows: rowsOf reads the rows of a range inside the view", "[ui][table][cov]") {
    Fixture f;
    std::vector<std::string> const columns{"name"};
    std::vector<table::RowId> got;
    auto read = f.rows->rowsOf(-2, 5, columns);  // clipped at the first row
    read.then([&got](std::expected<std::vector<ui::FetchedRow>, table::TableError> const& reply) {
        REQUIRE(reply.has_value());
        for (auto const& row : *reply) {
            got.push_back(row.key);
        }
    });
    f.h.settle();
    CHECK(got == std::vector<table::RowId>{id(0), id(1), id(2)});
    got.clear();
    auto tail = f.rows->rowsOf(8, 50, columns);  // clipped at the last row
    tail.then([&got](std::expected<std::vector<ui::FetchedRow>, table::TableError> const& reply) {
        REQUIRE(reply.has_value());
        for (auto const& row : *reply) {
            got.push_back(row.key);
        }
    });
    f.h.settle();
    CHECK(got == std::vector<table::RowId>{id(8), id(9)});
}

TEST_CASE(
    "cov engine rows: locateRow answers a row's view index, nothing for a row not in the view, and nothing once "
    "stopped",
    "[ui][table][cov]") {
    Fixture f;
    std::vector<std::optional<std::int64_t>> found;
    auto const ask = [&](table::RowId key, ::core::async::StopToken stop) {
        f.rows->locateRow(std::move(key), std::move(stop),
                          [&found](ui::RowLocation const& where) { found.push_back(where.value_or(std::nullopt)); });
        f.h.settle();
    };
    ask(id(4), {});
    ask(id(77), {});
    REQUIRE(found.size() == 2);
    CHECK(found.front() == 4);
    CHECK_FALSE(found.back().has_value());
    ::core::async::StopSource const source;
    f.rows->locateRow(id(4), source.get_token(), [&found](ui::RowLocation const&) { found.emplace_back(0); });
    source.request_stop();
    f.h.settle();
    CHECK(found.size() == 2);
}

TEST_CASE("cov engine rows: retained rows follow a row whose key changed", "[ui][table][cov]") {
    Fixture f;
    CHECK(f.rows->retain(std::vector<table::RowId>{id(3), id(5)}) == ui::RetainResult::Retained);
    CHECK(f.rows->retain(std::vector<table::RowId>{id(3)}) == ui::RetainResult::Retained);  // already held
    CHECK(f.rows->engine().retained().size() == 2);
    // A key that is not retained moves nothing.
    f.rows->moveKey(id(1), id(100));
    CHECK(f.rows->engine().retained().size() == 2);
    // A retained key becomes its new key in place.
    f.rows->moveKey(id(3), id(30));
    auto const held = f.rows->engine().retained();
    REQUIRE(held.size() == 2);
    CHECK(held[0] == id(30));
    CHECK(held[1] == id(5));
    // A new key already held replaces the old one by dropping it.
    f.rows->moveKey(id(30), id(5));
    REQUIRE(f.rows->engine().retained().size() == 1);
    CHECK(f.rows->engine().retained()[0] == id(5));
    f.rows->clearRetained();
    CHECK(f.rows->engine().retained().empty());
    f.rows->clearRetained();  // nothing to clear is not an error
    CHECK(f.rows->engine().retained().empty());
}

TEST_CASE("cov engine rows: a retained row the filter leaves out is counted and unmatched", "[ui][table][cov]") {
    Fixture f;
    f.filterTo("n7");
    REQUIRE(f.rows->retain(std::vector<table::RowId>{id(3)}) == ui::RetainResult::Retained);
    f.h.settle();
    CHECK(f.rows->unmatched(id(3)));
    CHECK_FALSE(f.rows->unmatched(id(7)));
    CHECK_FALSE(f.rows->unmatched(id(55)));
    auto const counts = f.rows->counts();
    CHECK(counts.retained == 1);
    CHECK(counts.source == 10);
    auto const view = f.rows->selectView();
    REQUIRE(view.has_value());
    CHECK(view->retained == std::vector<table::RowId>{id(3)});
    CHECK(view->filters.columns.contains("name"));
}

TEST_CASE("cov engine rows: rows over no source or no owner are refused", "[ui][table][cov]") {
    RowsFixture h;
    CHECK_THROWS_AS((EngineRows{nullptr, ui::ValueRowsSource::recordOf(), table::EngineOptions{.owner = &h.owner}}),
                    std::invalid_argument);
    auto const source = sourceOf(h, {col("name")});
    CHECK_THROWS_AS((EngineRows{source, ui::ValueRowsSource::recordOf(), table::EngineOptions{}}),
                    std::invalid_argument);
}

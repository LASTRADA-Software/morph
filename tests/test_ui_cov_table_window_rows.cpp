// SPDX-License-Identifier: Apache-2.0

// `WindowRows`' reads by key and by range: what each answers when the table is not described, when the call fails
// or the reply cannot be read, and how a long key list is split.

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <morph/table/data_source.hpp>
#include <morph/ui/table_rows.hpp>
#include <optional>
#include <string>
#include <vector>

#include "ui_table_support.hpp"

namespace {

namespace table = morph::table;
namespace ui = morph::ui;
using morph::ui::WindowRows;
using morph::ui::testing::RowsFixture;

using Keys = std::expected<std::vector<table::RowId>, table::TableError>;
using Rows = std::expected<std::vector<ui::FetchedRow>, table::TableError>;

table::RowId id(std::int64_t key) { return table::RowId{key}; }

std::string emptyPage(RowsFixture const& h) {
    return R"({"revision":")" + h.described.revision + R"(","columns":["id"],"keys":[],"rows":[],"total":0})";
}

struct Undescribed {
    RowsFixture h;
    std::shared_ptr<WindowRows> rows = std::make_shared<WindowRows>(h.session, h.options("samples"));
    Undescribed() { h.settle(); }
};
}  // namespace

TEST_CASE("cov window rows: without a description every read is unavailable", "[ui][table][cov]") {
    Undescribed u;
    std::optional<Keys> keys;
    u.rows->keysOf(0, 5).then([&keys](Keys const& got) { keys = got; });
    std::optional<Rows> fetched;
    u.rows->fetchRows({id(1)}, {"id"}).then([&fetched](Rows const& got) { fetched = got; });
    std::optional<ui::RowLocation> located;
    u.rows->locateRow(id(1), {}, [&located](ui::RowLocation const& got) { located = got; });
    std::optional<Rows> range;
    u.rows->rowsOf(0, 3, std::vector<std::string>{"id"}).then([&range](Rows const& got) { range = got; });
    u.h.settle();
    REQUIRE(keys.has_value());
    CHECK_FALSE(keys->has_value());
    REQUIRE(fetched.has_value());
    CHECK_FALSE(fetched->has_value());
    REQUIRE(located.has_value());
    CHECK_FALSE(located->has_value());
    REQUIRE(range.has_value());
    CHECK_FALSE(range->has_value());
    CHECK(u.h.dispatcher.count("Table.samples") == 0);  // nothing was asked of the server
}

TEST_CASE("cov window rows: a failed or unreadable fetch of keys is the range's error", "[ui][table][cov]") {
    RowsFixture h;
    auto const rows = h.windowRows("samples");
    std::vector<std::optional<Keys>> got(2);
    rows->keysOf(0, 5).then([&got](Keys const& result) { got[0] = result; });
    h.settle();
    h.dispatcher.fail(h.oldest("Table.samples"), "error", "the server is down");
    h.settle();
    REQUIRE(got[0].has_value());
    REQUIRE_FALSE(got[0]->has_value());
    CHECK(got[0]->error().message.contains("the server is down"));
    rows->keysOf(0, 5).then([&got](Keys const& result) { got[1] = result; });
    h.settle();
    h.replyPage("this is not a page");
    REQUIRE(got[1].has_value());
    CHECK_FALSE(got[1]->has_value());
}

TEST_CASE("cov window rows: a failed or unreadable fetch of rows is the fetch's error", "[ui][table][cov]") {
    RowsFixture h;
    auto const rows = h.windowRows("samples");
    std::vector<std::optional<Rows>> got(2);
    rows->fetchRows({id(1), id(2)}, {"id"}).then([&got](Rows const& result) { got[0] = result; });
    h.settle();
    h.dispatcher.fail(h.oldest("Table.samples"), "error", "boom");
    h.settle();
    REQUIRE(got[0].has_value());
    REQUIRE_FALSE(got[0]->has_value());
    CHECK(got[0]->error().message.contains("boom"));
    rows->fetchRows({id(1)}, {"id"}).then([&got](Rows const& result) { got[1] = result; });
    h.settle();
    h.replyPage("not json at all");
    REQUIRE(got[1].has_value());
    CHECK_FALSE(got[1]->has_value());
    // No keys is an empty answer and asks nothing.
    std::size_t const sent = h.dispatcher.count("Table.samples");
    std::optional<Rows> none;
    rows->fetchRows({}, {"id"}).then([&none](Rows const& result) { none = result; });
    h.settle();
    REQUIRE(none.has_value());
    REQUIRE(none->has_value());
    CHECK((*none)->empty());
    CHECK(h.dispatcher.count("Table.samples") == sent);
}

TEST_CASE("cov window rows: a long key list is fetched in chunks of maxKeys and answered whole", "[ui][table][cov]") {
    RowsFixture h;
    auto options = h.options("samples");
    options.maxKeys = 2;
    auto const rows = std::make_shared<WindowRows>(h.session, options);
    h.settle();
    rows->setDescription(h.described);
    h.settle();
    rows->release();
    h.settle();
    std::optional<Rows> got;
    rows->fetchRows({id(1), id(2), id(3), id(4), id(5)}, {"id"}).then([&got](Rows const& result) { got = result; });
    h.settle();
    CHECK(h.pending("Table.samples") == 3);
    std::vector<std::vector<std::int64_t>> const chunks{{1, 2}, {3, 4}, {5}};
    for (auto const& chunk : chunks) {
        std::string keys;
        std::string cells;
        for (auto const key : chunk) {
            keys += (keys.empty() ? "" : ",") + std::to_string(key);
            cells += (cells.empty() ? "" : ",") + ("[" + std::to_string(key) + "]");
        }
        std::string page = R"({"revision":")";
        page += h.described.revision;
        page += R"(","columns":["id"],"keys":[)";
        page += keys;
        page += R"(],"rows":[)";
        page += cells;
        page += R"(],"indices":[)";
        page += keys;
        page += R"(],"total":5})";
        h.replyPage(page);
    }
    REQUIRE(got.has_value());
    INFO((got->has_value() ? std::string{} : got->error().message));
    REQUIRE(got->has_value());
    CHECK((*got)->size() == 5);
}

TEST_CASE("cov window rows: locating a row by a failed, empty or index-less reply", "[ui][table][cov]") {
    RowsFixture h;
    auto const rows = h.windowRows("samples");
    std::vector<ui::RowLocation> answers;
    auto const ask = [&] {
        rows->locateRow(id(9), {}, [&answers](ui::RowLocation const& found) { answers.push_back(found); });
        h.settle();
    };
    ask();
    h.dispatcher.fail(h.oldest("Table.samples"), "error", "no route");
    h.settle();
    REQUIRE(answers.size() == 1);
    CHECK_FALSE(answers.back().has_value());
    ask();
    h.replyPage("garbage");
    REQUIRE(answers.size() == 2);
    CHECK_FALSE(answers.back().has_value());
    ask();
    h.replyPage(emptyPage(h));
    REQUIRE(answers.size() == 3);
    REQUIRE(answers.back().has_value());
    CHECK_FALSE(answers.back()->has_value());  // the view lacks the key
    ask();
    h.replyPage(R"({"revision":")" + h.described.revision +
                R"(","columns":["id"],"keys":[9],"rows":[[9]],"total":1})");
    REQUIRE(answers.size() == 4);
    CHECK_FALSE(answers.back().has_value());  // the row came without its index
    // A stopped read answers nothing.
    // NOLINTNEXTLINE(misc-const-correctness): request_stop is not const in every standard library.
    ::core::async::StopSource stop;
    rows->locateRow(id(9), stop.get_token(),
                    [&answers](ui::RowLocation const&) { answers.emplace_back(std::nullopt); });
    h.settle();
    stop.request_stop();
    h.replyPage(emptyPage(h));
    CHECK(answers.size() == 4);
}

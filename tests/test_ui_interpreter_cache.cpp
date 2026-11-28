// SPDX-License-Identifier: Apache-2.0

// The query cache's open, partial and opaque entries, and the session's read and watch that make no entry (spec
// table.md "The client's caches"; interpreter.md "Queries and the cache"). `T_Page` holds `T_Row` at `rows` beside
// an `awaiting` member that opens it and an `unloadedRows` member that makes it partial; `T_TablePage` holds `T_Row`
// opaquely, as a document query of `Table.<id>` does.
//
// Each case names the mutation it was seen to fail on.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <core/async/Cancellation.hpp>
#include <core/async/StopToken.hpp>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <format>
#include <functional>
#include <glaze/glaze.hpp>
#include <memory>
#include <morph/core/detail/result_walker.hpp>
#include <morph/core/wire.hpp>
#include <morph/table/description.hpp>
#include <morph/ui/interpreter.hpp>
#include <morph/ui/value.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "ui_interpreter_support.hpp"

namespace ui = morph::ui;
using ui::testing::Harness;

namespace {

ui::Catalog tableCatalog() {
    ui::Catalog catalog = ui::testing::corpusCatalog();
    auto const base = catalog.action;
    catalog.action = [base](std::string_view action) -> std::optional<ui::ActionInfo> {
        auto info = base(action);
        if (!info) {
            return info;
        }
        if (action == "T_Page") {
            info->records = {ui::ActionRecordInfo{.record = "T_Row",
                                                  .keyField = "code",
                                                  .paths = {"rows"},
                                                  .open = {"awaiting"},
                                                  .partial = {"unloadedRows"},
                                                  .opaque = false}};
        }
        if (action == "T_TablePage") {
            info->records = {ui::ActionRecordInfo{
                .record = "T_Row", .keyField = "code", .paths = {}, .open = {}, .partial = {}, .opaque = true}};
        }
        return info;
    };
    return catalog;
}

constexpr char const* kPageList = R"({
  "vocab": "ui/1", "id": "page",
  "models": { "sample": { "type": "Lims_SampleModel" } },
  "queries": { "page": { "model": "sample", "action": "T_Page" } },
  "view": { "kind": "text", "testId": "page", "text": { "fn": "format", "args": [ "{0}", { "query": "page" } ] } } })";

// Two queries of one table: a page and a distinct-values read.
constexpr char const* kTablePages = R"({
  "vocab": "ui/1", "id": "table",
  "models": { "sample": { "type": "Lims_SampleModel" } },
  "queries": {
    "page": { "model": "sample", "action": "T_TablePage", "body": { "obj": { "page": 0 } } },
    "distinct": { "model": "sample", "action": "T_TablePage", "body": { "obj": { "distinct": "name" } } } },
  "view": { "kind": "column", "children": [
    { "kind": "text", "testId": "page", "text": { "fn": "format", "args": [ "{0}", { "query": "page" } ] } },
    { "kind": "text", "testId": "distinct", "text": { "fn": "format", "args": [ "{0}", { "query": "distinct" } ] } } ] } })";

// One opaque query, whose reply is a generic page.
constexpr char const* kOneTablePage = R"({
  "vocab": "ui/1", "id": "one",
  "models": { "sample": { "type": "Lims_SampleModel" } },
  "queries": { "page": { "model": "sample", "action": "T_TablePage" } },
  "view": { "kind": "text", "testId": "page", "text": { "fn": "format", "args": [ "{0}", { "query": "page" } ] } } })";

morph::wire::ChangesBody changesOf(std::string record, std::vector<std::string> const& keys) {
    std::vector<glz::raw_json> raw;
    for (auto const& key : keys) {
        // NOLINTNEXTLINE(performance-inefficient-vector-operation): `raw` is a fixture list built once; sizing it first would not change what the case checks
        raw.emplace_back(key);
    }
    morph::wire::ChangesBody body;
    body.records.push_back(morph::effects::RecordChange{.record = std::move(record), .keys = std::move(raw)});
    return body;
}

std::string queryJson(ui::Screen const& screen, std::string_view query) {
    return ui::testing::readJson(screen, std::format(R"({{"query":"{}"}})", query));
}

ui::Value valueOf(std::string_view json) {
    auto parsed = ui::parseJson(json);
    REQUIRE(parsed);
    auto value = ui::valueFromJson(*parsed);
    REQUIRE(value);
    return *value;
}

}  // namespace

// Mutation: the cache ignoring `open`.
TEST_CASE("cache: an open entry is marked by a change to a key it does not hold", "[push][cache][table]") {
    Harness h{tableCatalog()};
    auto const screen = h.mount(kPageList);
    REQUIRE(h.dispatcher.calls.size() == 1);
    h.reply(0, R"({"rows":[{"code":1}],"awaiting":["recipe"],"unloadedRows":[]})");
    h.session.applyChanges(changesOf("T_Row", {"99"}));
    h.settle();
    CHECK(h.dispatcher.calls.size() == 2);  // refetched
    h.reply(1, R"({"rows":[{"code":1}],"awaiting":[],"unloadedRows":[]})");
    h.session.applyChanges(changesOf("T_Row", {"99"}));
    h.settle();
    CHECK(h.dispatcher.calls.size() == 2);  // not open any more: key 99 is not held
}

// Mutation: the cache ignoring `partial`.
TEST_CASE("cache: a partial entry is not merged into", "[push][cache][table]") {
    Harness h{tableCatalog()};
    auto const screen = h.mount(kPageList);
    h.reply(0, R"({"rows":[{"code":1,"qty":1}],"awaiting":[],"unloadedRows":[{"index":0,"levels":["recipe"]}]})");
    h.session.actionSucceeded("Lims_SampleModel", "Push_EditRow", valueOf(R"({"code":1,"qty":5})"),
                              ui::ReplyUse::Applied, "private#1");
    h.settle();
    CHECK(h.dispatcher.calls.size() == 2);  // marked and refetched
    CHECK(queryJson(*screen, "page").contains(R"("qty":1)"));
    h.reply(1, R"({"rows":[{"code":1,"qty":1}],"awaiting":[],"unloadedRows":[]})");
    h.session.actionSucceeded("Lims_SampleModel", "Push_EditRow", valueOf(R"({"code":1,"qty":5})"),
                              ui::ReplyUse::Applied, "private#1");
    h.settle();
    CHECK(h.dispatcher.calls.size() == 2);  // merged, not fetched
    CHECK(queryJson(*screen, "page").contains(R"("qty":5)"));
}

// Mutation: the cache looking for a key member in an opaque entry.
TEST_CASE("cache: an opaque entry is marked by every change and never merged", "[push][cache][table]") {
    Harness h{tableCatalog()};
    auto const screen = h.mount(kTablePages);
    REQUIRE(h.dispatcher.calls.size() == 2);
    for (std::size_t call = 0; call < 2; ++call) {
        if (h.dispatcher.calls[call].body.contains("distinct")) {
            h.reply(call, R"({"distinct":{"values":["a"]}})");
        } else {
            h.reply(call, R"({"columns":["code"],"keys":[1],"rows":[[1]],"total":1})");
        }
    }
    h.session.applyChanges(changesOf("T_Row", {"99"}));
    h.settle();
    REQUIRE(h.dispatcher.calls.size() == 4);  // both marked
    h.reply(2, R"({"columns":["code"],"keys":[1],"rows":[[1]],"total":1})");
    h.reply(3, R"({"columns":["code"],"keys":[1],"rows":[[1]],"total":1})");
    auto const before = queryJson(*screen, "page");
    h.session.actionSucceeded("Lims_SampleModel", "Push_EditRow", valueOf(R"({"code":1,"qty":5})"),
                              ui::ReplyUse::Applied, "private#1");
    h.settle();
    CHECK(h.dispatcher.calls.size() == 6);  // an updates effect only marks
    CHECK(queryJson(*screen, "page") == before);
}

// Mutation: `read` going through `acquire` (the cache grows by one).
TEST_CASE("cache: a read makes no entry", "[push][cache][table]") {
    Harness h{tableCatalog()};
    auto const channel = h.dispatcher.bind("Lims_SampleModel", morph::bridge::BindSharing::Private, "");
    auto const entries = h.session.cacheSize();
    std::optional<std::string> text;
    auto read = h.session.read(channel, "T_Page", valueOf(R"({"page":{"offset":0}})"));
    read.then([&](std::string const& reply) { text = reply; }).onError([](std::exception_ptr const&) {});
    h.settle();
    REQUIRE(h.dispatcher.calls.size() == 1);
    CHECK(h.dispatcher.calls[0].action == "T_Page");
    CHECK(h.dispatcher.calls[0].body == R"({"page":{"offset":0}})");
    std::string const reply = R"({"rows":[ {"code":1} ],"awaiting":[]})";
    h.reply(0, reply);
    CHECK(text == reply);
    CHECK(h.session.cacheSize() == entries);

    // NOLINTNEXTLINE(misc-const-correctness): request_stop is const on libstdc++ only; the standard declares it non-const.
    ::core::async::StopSource stop;
    bool cancelled = false;
    bool resolved = false;
    auto stopped = h.session.read(channel, "T_Page", valueOf("{}"), stop.get_token());
    stopped.then([&](std::string const&) { resolved = true; }).onError([&](std::exception_ptr const& error) {
        try {
            std::rethrow_exception(error);
        } catch (core::async::OperationCancelled const&) {
            cancelled = true;
            // NOLINTNEXTLINE(bugprone-empty-catch): the case asserts `cancelled` for the cancellation and ignores any other error, which `resolved` and `cancelled` stay false for
        } catch (...) {
        }
    });
    h.settle();
    REQUIRE(h.dispatcher.calls.size() == 2);
    CHECK(h.dispatcher.calls[1].options.stop.stop_possible());
    stop.request_stop();
    h.settle();
    CHECK(cancelled);
    h.reply(1, reply);
    CHECK_FALSE(resolved);
    CHECK(h.session.cacheSize() == entries);
}

// Mutation: `read` parsing the reply through `valueFromJson` and resolving with `canonicalJson` of the result (the
// completion fails: a number with a fraction is refused).
TEST_CASE("cache: a read's text keeps a fractional number", "[push][cache][table]") {
    Harness h{tableCatalog()};
    auto const channel = h.dispatcher.bind("Lims_SampleModel", morph::bridge::BindSharing::Private, "");
    morph::table::TableDescription description;
    description.columns.push_back(morph::table::TableColumn{});
    description.columns.back().info = morph::table::ColumnInfo{.id = "n", .kind = morph::table::ColumnKind::Number};
    morph::table::GenericPage page;
    page.columns = {"n"};
    page.keys = {morph::table::RowId{std::int64_t{1}}};
    page.rows = {{morph::table::Cell{2.5}}};
    page.total = 1;
    auto const written = glz::write_json(page);
    REQUIRE(written.has_value());
    std::optional<std::string> text;
    bool failed = false;
    auto read = h.session.read(channel, "T_Page", valueOf("{}"));
    read.then([&](std::string const& reply) { text = reply; }).onError([&](std::exception_ptr const&) {
        failed = true;
    });
    h.settle();
    h.reply(0, *written);
    CHECK_FALSE(failed);
    REQUIRE(text.has_value());
    auto const decoded = morph::table::readGenericPage(*text, description);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->rows.size() == 1);
    REQUIRE(std::holds_alternative<double>(decoded->rows[0][0]));
    CHECK(std::get<double>(decoded->rows[0][0]) == 2.5);
}

// Mutations: holders not counted in the watch set; the registration released when the first copy dies (the
// `unwatch` goes out while a copy lives).
TEST_CASE("cache: a watch without an entry joins the watch set", "[push][cache][table]") {
    Harness h{tableCatalog(), true};
    std::vector<std::optional<std::vector<glz::raw_json>>> heard;
    std::optional<ui::WatchHolder> copy;
    {
        auto watching = h.session.watchRecord(
            "T_Row", [&](morph::effects::RecordChange const& change) { heard.push_back(change.keys); });
        watching.then([&](ui::WatchHolder const& holder) { copy = holder; });
        h.settle();
    }
    h.settle();
    REQUIRE(copy.has_value());
    REQUIRE(h.watches.size() == 1);
    CHECK(h.watches[0].kind == "watch");
    CHECK(h.watches[0].body.contains("T_Row"));
    auto const names = [&] {
        auto const set = h.session.watchSet();
        return std::ranges::find(set.records, "T_Row") != set.records.end();
    };
    CHECK(names());

    h.session.deliverEvent(morph::wire::EventBody{.topic = std::string{morph::wire::kResyncTopic}, .payload = {}});
    h.settle();
    REQUIRE(heard.size() == 1);
    CHECK_FALSE(heard[0].has_value());
    h.session.applyChanges(changesOf("T_Row", {"7", "8"}));
    h.settle();
    REQUIRE(heard.size() == 2);
    REQUIRE(heard[1].has_value());
    CHECK(heard[1]->size() == 2);

    auto second = copy;
    copy.reset();
    h.settle();
    CHECK(names());
    auto const unwatches = [&] {
        return std::ranges::count_if(h.watches, [](auto const& sent) { return sent.kind == "unwatch"; });
    };
    CHECK(unwatches() == 0);
    second.reset();
    h.settle();
    CHECK_FALSE(names());
    CHECK(unwatches() == 1);
}

// Mutations: `settleReply` using `valueFromJson` for every entry (the opaque entry is in error); the table-aware
// decode used for every entry (the `T_Page` entry settles); the fraction written by `std::format("{}")`, which
// spells 0.0001 as `1e-04` (the cell is that text, not a decimal).
TEST_CASE("cache: an opaque entry keeps a fractional number", "[push][cache][table]") {
    constexpr char const* kReply =
        R"({"revision":"r","columns":["n","big","i","small","quarter"],"keys":[1],"rows":[[2.5,1e300,7,0.0001,0.00025]],"total":1,"totalKind":"exact","offset":0})";
    {
        Harness h{tableCatalog()};
        auto const screen = h.mount(kOneTablePage);
        h.reply(0, kReply);
        auto const shown = queryJson(*screen, "page");
        INFO(shown);
        // The shown value is read back with the strict reader: its decimals are exact objects, its text is text.
        auto parsed = ui::parseJson(shown);
        REQUIRE(parsed);
        auto value = ui::valueFromJson(*parsed);
        REQUIRE(value.has_value());
        auto const* const page = value->asRecord();
        REQUIRE(page != nullptr);
        auto const* const rows = page->at("rows").asList();
        REQUIRE(rows != nullptr);
        auto const* const row = rows->at(0).asList();
        REQUIRE(row != nullptr);
        // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): `decimalAt(at, expected, places)`: a column index, the expected value and the decimal places read differently at each call, and a swap would fail the lambda's own checks
        auto const decimalAt = [&](std::size_t at, double expected, std::uint32_t places) {
            INFO("cell " << at);
            auto const* const decimal = row->at(at).asDecimal();
            REQUIRE(decimal != nullptr);
            CHECK(std::abs(decimal->value.toDouble() - expected) < 1e-12);
            CHECK(decimal->value.getDecimalPlaces().value == places);
        };
        decimalAt(0, 2.5, 1);
        decimalAt(3, 0.0001, 4);
        decimalAt(4, 0.00025, 5);
        REQUIRE(row->at(1).asString() != nullptr);
        CHECK(*row->at(1).asString() == std::format("{}", 1e300));
        REQUIRE(row->at(2).asInt() != nullptr);
        CHECK(*row->at(2).asInt() == 7);
    }
    {
        Harness h{tableCatalog()};
        auto const screen = h.mount(kPageList);
        h.reply(0, kReply);
        CHECK(queryJson(*screen, "page") == "null");
        CHECK(ui::testing::readJson(*screen, R"({"error":"page"})").contains("the reply is not a document value"));
    }
}

namespace cache_walk {

struct WalkRow {
    std::int64_t code = 0;
};

struct Wrapped {
    morph::table::Page<WalkRow> page;
};

}  // namespace cache_walk

// Mutation: the walk recording no holder (resolution gives the record no open or partial member).
TEST_CASE("cache: a page declares its open and partial members to the result walk", "[push][cache][table]") {
    auto const holders = morph::model::detail::walkHolders<cache_walk::Wrapped>();
    REQUIRE(holders.size() == 1);
    CHECK(holders[0].path == "page");
    CHECK(holders[0].open == std::vector<std::string>{"awaiting", "distinct"});
    CHECK(holders[0].partial == "unloadedRows");
}

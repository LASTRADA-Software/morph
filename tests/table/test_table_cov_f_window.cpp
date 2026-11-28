// SPDX-License-Identifier: Apache-2.0
//
// The page window of a server-mode table, over each row representation it holds: how a keyed window answers a
// change of its record, which replies it ignores, and what it drops when the total shrinks or too many pages are
// held.

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cstddef>
#include <cstdint>
#include <morph/table/data_source.hpp>
#include <morph/table/description.hpp>
#include <morph/table/query.hpp>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using namespace morph::table;

namespace {

RowId key(std::int64_t value) { return RowId{value}; }

// One row of a reply: its key, whether its content was edited since the page was read, and its view row.
struct Entry {
    std::int64_t key;
    bool edited;
    std::int64_t index;
};

std::int64_t contentOf(Entry const& entry) { return entry.key + (entry.edited ? 1000 : 0); }

// A window of typed rows whose row is its content, and whose key is the content modulo 1000.
struct IntRows {
    using Window = PageWindow<int>;

    static Window make(std::int64_t pageSize, std::size_t maxPages, bool keyed = true, std::size_t maxKeys = 10) {
        return Window{
            pageSize, maxPages,
            keyed ? RowKey<int>{[](int const& row) { return RowId{std::int64_t{row % 1000}}; }} : RowKey<int>{},
            maxKeys};
    }

    static Page<int> page(std::int64_t first, std::int64_t count, std::optional<std::int64_t> total) {
        Page<int> out;
        for (std::int64_t at = 0; at < count; ++at) {
            out.rows.push_back(static_cast<int>(first + at));
        }
        out.total = total;
        return out;
    }

    static Page<int> keyed(std::vector<Entry> const& entries, std::optional<std::int64_t> total) {
        Page<int> out;
        for (auto const& entry : entries) {
            out.rows.push_back(static_cast<int>(contentOf(entry)));
            out.indices.push_back(entry.index);
        }
        out.total = total;
        return out;
    }

    static std::vector<PageFetch> visible(Window& window, std::int64_t first, std::int64_t count) {
        return window.visible(first, count);
    }

    static std::optional<std::int64_t> at(Window const& window, std::int64_t index) {
        auto const* const row = window.row(index);
        return row == nullptr ? std::nullopt : std::optional<std::int64_t>{*row};
    }
};

// A window of generic rows with one integer column, `a`, holding the row's content; its key is the row's key.
struct CellRows {
    using Window = PageWindow<GenericRow>;

    static Window make(std::int64_t pageSize, std::size_t maxPages, bool = true, std::size_t maxKeys = 10) {
        return Window{pageSize, maxPages, maxKeys};
    }

    static GenericPage page(std::int64_t first, std::int64_t count, std::optional<std::int64_t> total) {
        GenericPage out;
        out.columns = {"a"};
        for (std::int64_t at = 0; at < count; ++at) {
            out.keys.emplace_back(first + at);
            out.rows.push_back({Cell{first + at}});
        }
        out.total = total;
        return out;
    }

    static GenericPage keyed(std::vector<Entry> const& entries, std::optional<std::int64_t> total) {
        GenericPage out;
        out.columns = {"a"};
        for (auto const& entry : entries) {
            out.keys.emplace_back(entry.key);
            out.rows.push_back({Cell{contentOf(entry)}});
            out.indices.push_back(entry.index);
        }
        out.total = total;
        return out;
    }

    static std::vector<PageFetch> visible(Window& window, std::int64_t first, std::int64_t count) {
        std::vector<std::string> const columns{"a"};
        return window.visible(first, count, columns);
    }

    static std::optional<std::int64_t> at(Window const& window, std::int64_t index) {
        auto const* const cell = window.cell(index, "a");
        if (cell == nullptr || !std::holds_alternative<std::int64_t>(*cell)) {
            return std::nullopt;
        }
        return std::get<std::int64_t>(*cell);
    }
};

// A window of pages of four rows, holding pages 0 and 1: the keys 0 to 7, in a view of eight rows.
template <typename Rows>
struct Held {
    explicit Held(std::size_t maxPages = 3, std::size_t maxKeys = 10)
        : window{Rows::make(4, maxPages, true, maxKeys)} {
        auto const fetches = Rows::visible(window, 0, 4);
        REQUIRE(fetches.size() == 2);
        REQUIRE(window.accept(fetches[0], Rows::page(0, 4, 8)));
        REQUIRE(window.accept(fetches[1], Rows::page(4, 4, 8)));
    }

    Rows::Window window;
};

template <typename Window>
RowsFetch rowsPlan(Window& window, std::vector<RowId> const& keys) {
    auto const plan = window.refresh(keys);
    REQUIRE(plan.kind == Refresh::Rows);
    return plan.rows;
}

}  // namespace

TEMPLATE_TEST_CASE("a keyed reply patches the rows it names and leaves the others", "[covf][table][window]", IntRows,
                   CellRows) {
    Held<TestType> held;
    auto const fetch = rowsPlan(held.window, {key(5)});
    CHECK(fetch.keys == std::vector<RowId>{key(5), key(0)});
    auto const patched = held.window.patch(fetch, TestType::keyed({{5, true, 5}, {0, false, 0}}, 8));
    CHECK(patched == std::vector<std::int64_t>{0, 5});
    CHECK(TestType::at(held.window, 5) == 1005);
    CHECK(TestType::at(held.window, 4) == 4);
}

TEMPLATE_TEST_CASE("a keyed reply that shows the rows moved invalidates the window instead of patching",
                   "[covf][table][window]", IntRows, CellRows) {
    struct Shape {
        char const* name = nullptr;
        std::vector<Entry> entries;
        std::optional<std::int64_t> total;
    };
    auto const shape = GENERATE(Catch::Generators::values<Shape>({
        {"a held key at another view row", {{5, true, 6}, {0, false, 0}}, 8},
        {"a probe the reply omits", {{5, true, 5}}, 8},
        {"an unheld key inside a held page", {{5, true, 5}, {0, false, 0}, {99, false, 2}}, 8},
        {"a total that differs", {{5, true, 5}, {0, false, 0}}, 9},
    }));
    CAPTURE(shape.name);
    Held<TestType> held;
    auto const before = held.window.revision();
    auto const fetch = rowsPlan(held.window, {key(5)});
    CHECK(held.window.patch(fetch, TestType::keyed(shape.entries, shape.total)).empty());
    CHECK(held.window.revision() == before + 1);
    CHECK(TestType::visible(held.window, 0, 4).size() == 2);
}

TEMPLATE_TEST_CASE("a keyed reply whose undecided count differs invalidates, and one with ragged indices does too",
                   "[covf][table][window]", IntRows, CellRows) {
    Held<TestType> held;
    auto const fetch = rowsPlan(held.window, {key(5)});
    auto undecided = TestType::keyed({{5, true, 5}, {0, false, 0}}, 8);
    undecided.undecided = 2;
    auto const undecidedPatch = held.window.patch(fetch, std::move(undecided));
    CHECK(undecidedPatch.empty());
    CHECK(held.window.revision() == 1);

    auto const ragged = rowsPlan(held.window, {key(5)});
    auto reply = TestType::keyed({{5, true, 5}, {0, false, 0}}, 8);
    reply.indices.pop_back();
    auto const raggedPatch = held.window.patch(ragged, std::move(reply));
    CHECK(raggedPatch.empty());
    CHECK(held.window.revision() == 2);
}

TEMPLATE_TEST_CASE("a keyed reply ignores a row a later fetch asked for again, and a row outside the held pages",
                   "[covf][table][window]", IntRows, CellRows) {
    Held<TestType> held;
    auto const first = rowsPlan(held.window, {key(5)});
    auto const second = rowsPlan(held.window, {key(5)});
    CHECK(second.sequence > first.sequence);
    // The first reply's rows for keys 5 and 0 are older than the second fetch's: neither is applied.
    auto const stale = held.window.patch(first, TestType::keyed({{5, true, 5}, {0, false, 0}, {50, false, 50}}, 8));
    CHECK(stale.empty());
    CHECK(TestType::at(held.window, 5) == 5);
    CHECK(held.window.revision() == 0);
    // A key past the held pages (and one with no view row) is not held, so nothing is invalidated or replaced.
    auto const outside =
        held.window.patch(second, TestType::keyed({{5, true, 5}, {0, false, 0}, {77, false, 40}, {78, false, -1}}, 8));
    CHECK(outside == std::vector<std::int64_t>{0, 5});
    CHECK(held.window.revision() == 0);
}

TEMPLATE_TEST_CASE("a keyed reply of a past query patches nothing", "[covf][table][window]", IntRows, CellRows) {
    Held<TestType> held;
    auto const fetch = rowsPlan(held.window, {key(5)});
    held.window.reset();
    CHECK(held.window.patch(fetch, TestType::keyed({{5, true, 5}, {0, false, 0}}, 8)).empty());
}

TEST_CASE("a window without keys cannot patch and answers a change with its pages", "[covf][table][window]") {
    auto window = IntRows::make(4, 3, false);
    auto const fetches = IntRows::visible(window, 0, 4);
    REQUIRE(window.accept(fetches[0], IntRows::page(0, 4, 8)));
    CHECK(window.patch(RowsFetch{}, IntRows::keyed({{1, false, 0}}, 8)).empty());
    CHECK(window.refresh({{key(1)}}).kind == Refresh::Pages);
    CHECK(window.revision() == 1);
    CHECK(window.owed().kind == Refresh::Pages);
    CHECK(window.owed().kind == Refresh::Nothing);
}

TEMPLATE_TEST_CASE(
    "a change asks for nothing while no row is held, for pages while rows wait on a level, and for "
    "pages when too many keys are asked",
    "[covf][table][window]", IntRows, CellRows) {
    auto empty = TestType::make(4, 3);
    CHECK(empty.refresh({{key(1)}}).kind == Refresh::Nothing);
    CHECK(empty.owed().kind == Refresh::Nothing);

    auto waiting = TestType::make(4, 3);
    auto const fetch = TestType::visible(waiting, 100, 4);
    REQUIRE_FALSE(fetch.empty());
    auto reply = TestType::page(100, 4, 200);
    reply.awaiting = {"slow"};
    reply.failed = {"broken"};
    reply.undecided = 3;
    auto const outcome = waiting.accept(fetch[0], std::move(reply));
    REQUIRE(outcome);
    CHECK(waiting.awaiting().size() == 1);
    CHECK(waiting.failed().size() == 1);
    CHECK(waiting.undecided() == 3);
    CHECK(waiting.refresh({{key(1)}}).kind == Refresh::Pages);

    Held<TestType> narrow{3, 1};
    CHECK(narrow.window.refresh({{key(1)}}).kind == Refresh::Pages);
    CHECK(narrow.window.revision() == 1);
}

TEMPLATE_TEST_CASE("changes that arrive while a page is in flight are owed once the page lands",
                   "[covf][table][window]", IntRows, CellRows) {
    auto window = TestType::make(4, 3);
    auto const fetches = TestType::visible(window, 0, 4);
    REQUIRE(fetches.size() == 2);
    REQUIRE(window.accept(fetches[0], TestType::page(0, 4, 8)));
    CHECK(window.pending());
    CHECK(window.refresh({{key(3)}}).kind == Refresh::Rows);
    // The page in flight keeps the keys owed: asking again returns the same plan, and still holds them.
    auto const owed = window.owed();
    CHECK(owed.kind == Refresh::Rows);
    CHECK(owed.rows.keys == std::vector<RowId>{key(3), key(0)});
    CHECK(window.owed().kind == Refresh::Rows);
    REQUIRE(window.accept(fetches[1], TestType::page(4, 4, 8)));
    CHECK(window.owed().kind == Refresh::Rows);
    CHECK(window.owed().kind == Refresh::Nothing);
}

TEMPLATE_TEST_CASE("the farthest page goes when more pages are held than allowed", "[covf][table][window]", IntRows,
                   CellRows) {
    auto window = TestType::make(4, 2);
    for (auto const& fetch : TestType::visible(window, 0, 4)) {
        REQUIRE(window.accept(fetch, TestType::page(fetch.request.offset, 4, std::nullopt)));
    }
    CHECK(window.pagesHeld() == 2);
    auto const middle = TestType::visible(window, 8, 4);
    REQUIRE(middle.size() == 2);
    for (auto const& fetch : middle) {
        REQUIRE(window.accept(fetch, TestType::page(fetch.request.offset, 4, std::nullopt)));
    }
    CHECK(window.pagesHeld() == 2);
    CHECK(TestType::at(window, 0) == std::nullopt);
    CHECK(TestType::at(window, 8) == 8);
}

TEMPLATE_TEST_CASE("a total below the held rows truncates the last page and drops the pages past it",
                   "[covf][table][window]", IntRows, CellRows) {
    auto window = TestType::make(4, 4);
    auto const fetches = TestType::visible(window, 0, 4);
    REQUIRE(fetches.size() == 2);
    REQUIRE(window.accept(fetches[0], TestType::page(0, 4, std::nullopt)));
    REQUIRE(window.accept(fetches[1], TestType::page(4, 4, 6)));
    CHECK(TestType::at(window, 5) == 5);
    CHECK(TestType::at(window, 6) == std::nullopt);
    // The total moved the other page out of date: it is fetched again.
    auto const again = TestType::visible(window, 0, 4);
    REQUIRE(again.size() == 1);
    CHECK(again[0].request.offset == 0);
}

TEMPLATE_TEST_CASE("a total reached by the first page ends the fetches of the pages past it", "[covf][table][window]",
                   IntRows, CellRows) {
    auto window = TestType::make(4, 4);
    auto const fetches = TestType::visible(window, 0, 4);
    REQUIRE(fetches.size() == 2);
    REQUIRE(window.accept(fetches[0], TestType::page(0, 3, 3)));
    CHECK_FALSE(window.pending());
    CHECK_FALSE(window.accept(fetches[1], TestType::page(4, 4, 3)));
    CHECK(TestType::visible(window, 0, 4).empty());
}

TEMPLATE_TEST_CASE("a page the server cut short is fetched from where it ended", "[covf][table][window]", IntRows,
                   CellRows) {
    auto window = TestType::make(4, 4);
    auto const first = TestType::visible(window, 0, 4);
    REQUIRE(first.size() == 2);
    REQUIRE(window.accept(first[0], TestType::page(0, 2, std::nullopt)));
    auto const rest = TestType::visible(window, 0, 4);
    REQUIRE(rest.size() == 1);
    CHECK(rest[0].request.offset == 2);
    CHECK(rest[0].request.limit == 2);
    REQUIRE(window.accept(rest[0], TestType::page(2, 2, std::nullopt)));
    CHECK(TestType::at(window, 3) == 3);
    // An empty reply ends a page that is still short.
    REQUIRE(window.accept(first[1], TestType::page(4, 0, std::nullopt)));
    CHECK(TestType::visible(window, 0, 4).empty());
}

TEMPLATE_TEST_CASE("a reply of an older revision leaves the totals of a newer one", "[covf][table][window]", IntRows,
                   CellRows) {
    auto window = TestType::make(4, 8);
    auto const old = TestType::visible(window, 0, 4);
    REQUIRE(old.size() == 2);
    window.invalidate();
    auto const far = TestType::visible(window, 400, 4);
    REQUIRE_FALSE(far.empty());
    REQUIRE(window.accept(far[0], TestType::page(far[0].request.offset, 4, 1000)));
    CHECK(window.total() == 1000);
    REQUIRE(window.accept(old[0], TestType::page(0, 4, 5)));
    CHECK(window.total() == 1000);
}

TEMPLATE_TEST_CASE("a fetch that was cancelled or superseded is forgotten, and a failure of an old query is ignored",
                   "[covf][table][window]", IntRows, CellRows) {
    auto window = TestType::make(4, 4);
    auto const fetches = TestType::visible(window, 0, 4);
    REQUIRE(fetches.size() == 2);
    CHECK(window.superseded().empty());
    auto const moved = TestType::visible(window, 400, 4);
    CHECK(window.superseded().size() == 2);
    window.cancelled(fetches[0]);
    CHECK(window.superseded().size() == 1);
    window.cancelled(fetches[0]);
    CHECK(window.superseded().size() == 1);
    CHECK_FALSE(moved.empty());
    window.reset();
    CHECK_FALSE(
        window.fail(fetches[1], TableError{.code = TableErrorCode::Unavailable, .column = {}, .message = "x"}));
    CHECK_FALSE(window.error().has_value());
}

TEMPLATE_TEST_CASE("the levels a row still waits on are held with the row and replaced when it is patched",
                   "[covf][table][window]", IntRows, CellRows) {
    auto window = TestType::make(4, 4);
    auto const fetches = TestType::visible(window, 0, 4);
    REQUIRE(fetches.size() == 2);
    auto reply = TestType::page(0, 4, 8);
    reply.unloadedRows = {UnloadedRow{.index = 1, .levels = {"slow"}, .failed = {}},
                          UnloadedRow{.index = 3, .levels = {"a", "b"}, .failed = {"b"}}};
    auto const outcome = window.accept(fetches[0], std::move(reply));
    REQUIRE(outcome);
    REQUIRE(window.accept(fetches[1], TestType::page(4, 4, 8)));
    CHECK(window.unloadedLevels(0).levels.empty());
    REQUIRE(window.unloadedLevels(1).levels.size() == 1);
    CHECK(window.unloadedLevels(1).levels[0] == "slow");
    REQUIRE(window.unloadedLevels(3).failed.size() == 1);
    CHECK(window.unloadedLevels(100).levels.empty());
    CHECK(window.unloadedLevels(-1).levels.empty());

    auto const fetch = rowsPlan(window, {key(1)});
    auto patch = TestType::keyed({{1, true, 1}, {0, false, 0}}, 8);
    patch.unloadedRows = {UnloadedRow{.index = 0, .levels = {"later"}, .failed = {}}};
    auto const patched = window.patch(fetch, std::move(patch));
    CHECK(patched == std::vector<std::int64_t>{0, 1});
    CHECK(window.unloadedLevels(0).levels.empty());
    REQUIRE(window.unloadedLevels(1).levels.size() == 1);
    CHECK(window.unloadedLevels(1).levels[0] == "later");
    CHECK(window.unloadedLevels(3).levels.size() == 2);
}

TEMPLATE_TEST_CASE("the keys a window remembers a fetch for follow the rows it holds", "[covf][table][window]",
                   IntRows, CellRows) {
    auto window = TestType::make(4, 1);
    for (auto const& fetch : TestType::visible(window, 0, 4)) {
        REQUIRE(window.accept(fetch, TestType::page(fetch.request.offset, 4, std::nullopt)));
    }
    static_cast<void>(rowsPlan(window, {key(1), key(2)}));
    CHECK(window.keysRemembered() > 0);
    CHECK(window.keysRemembered() <= 4);
    auto const far = TestType::visible(window, 400, 4);
    REQUIRE_FALSE(far.empty());
    for (auto const& fetch : far) {
        REQUIRE(window.accept(fetch, TestType::page(fetch.request.offset, 4, std::nullopt)));
    }
    CHECK(window.pagesHeld() == 1);
    CHECK(window.keysRemembered() == 0);
}

TEST_CASE("a keyed reply whose keys and rows differ in number invalidates the window", "[covf][table][window]") {
    auto window = CellRows::make(4, 3);
    auto const fetches = CellRows::visible(window, 0, 4);
    auto reply = CellRows::page(0, 4, 8);
    reply.keys.pop_back();
    auto const outcome = window.accept(fetches[0], std::move(reply));
    REQUIRE(outcome);
    REQUIRE(window.error().has_value());
    CHECK(window.error()->code == TableErrorCode::InvalidValue);
    CHECK(window.revision() == 1);
}

TEST_CASE("a column reply is merged by key, and invalidates the window when the rows differ",
          "[covf][table][window]") {
    auto window = CellRows::make(4, 3);
    std::vector<std::string> const one{"a"};
    std::vector<std::string> const two{"a", "b"};
    for (auto const& fetch : window.visible(0, 4, one)) {
        REQUIRE(window.accept(fetch, CellRows::page(fetch.request.offset, 4, std::nullopt)));
    }
    auto const more = window.visible(0, 4, two);
    REQUIRE(more.size() == 2);
    CHECK(more[0].request.limit == 4);
    CHECK(more[0].columns == std::vector<std::string>{"b"});
    GenericPage reply;
    reply.columns = {"b"};
    for (std::int64_t at = 0; at < 4; ++at) {
        reply.keys.emplace_back(at);
        reply.rows.push_back({Cell{at * 10}});
    }
    reply.unloadedRows = {UnloadedRow{.index = 2, .levels = {"slow"}, .failed = {}},
                          UnloadedRow{.index = 9, .levels = {"slow"}, .failed = {}}};
    auto const outcome = window.accept(more[0], std::move(reply));
    REQUIRE(outcome);
    REQUIRE(window.cell(3, "b") != nullptr);
    CHECK(std::get<std::int64_t>(*window.cell(3, "b")) == 30);
    CHECK(window.cell(3, "zz") == nullptr);
    CHECK(window.cell(1000, "a") == nullptr);
    REQUIRE(window.unloadedLevels(2).levels.size() == 1);
    CHECK(window.revision() == 0);

    auto const wider = window.visible(0, 4, std::vector<std::string>{"a", "b", "c"});
    REQUIRE(wider.size() == 1);
    GenericPage other;
    other.columns = {"c"};
    other.keys = {key(9), key(1), key(2), key(3)};
    for (int at = 0; at < 4; ++at) {
        other.rows.push_back({Cell{std::int64_t{at}}});
    }
    auto const widerOutcome = window.accept(wider[0], std::move(other));
    REQUIRE(widerOutcome);
    CHECK(window.revision() == 1);
}

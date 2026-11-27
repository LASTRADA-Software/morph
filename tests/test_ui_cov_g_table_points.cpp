// SPDX-License-Identifier: Apache-2.0

// A table that keeps its state shows its layout and saved view at the customization points that name it; a point that
// names no such table, or is written a value of the wrong shape, is left alone, and a save the store refuses is said
// at every table of that key.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <memory>
#include <morph/table/state_store.hpp>
#include <string>
#include <string_view>

#include "ui_table_support.hpp"

namespace {

namespace table = morph::table;
using morph::ui::Value;
using morph::ui::testing::chooseMenu;
using morph::ui::testing::GatedTableStateStore;
using morph::ui::testing::kModelTable;
using morph::ui::testing::kThree;
using morph::ui::testing::readJson;
using morph::ui::testing::replyDescribe;
using morph::ui::testing::rowsDoc;
using morph::ui::testing::statusOf;
using morph::ui::testing::storeExecutor;
using morph::ui::testing::TableHarness;
using morph::ui::testing::tableOf;
using namespace std::chrono_literals;

constexpr std::string_view kPoints =
    R"("customize":{"cols":{"kind":"columns","of":"list"},"views":{"kind":"saved","of":"list"},)"
    R"("anon":{"kind":"saved"},)"
    R"("pick":{"kind":"choice","options":["a","b"]}},)";

constexpr std::string_view kStoredView =
    R"({"version":1,"layout":{"order":["id","name","c3"],"hidden":[],"pinned":0},"view":"Mine"})";

std::string modelDocument() {
    std::string text{kModelTable};
    text.insert(text.find(R"("models":)"), kPoints);
    return text;
}

std::string rowsDocument() {
    std::string text = rowsDoc(kThree);
    text.insert(text.find(R"("state":)"), kPoints);
    return text;
}

struct Written {
    std::string_view name;
    std::string_view json;
    bool accepted;
    std::string_view view;
};

}  // namespace

TEST_CASE("covg table points: a value of the wrong shape is refused and one for no table is a preference",
          "[covg][ui][table][state]") {
    auto const& c = GENERATE(values<Written>({
        {"views", R"("Other")", true, R"("Other")"},
        {"views", "null", true, "null"},
        {"views", "7", false, R"("Mine")"},
        {"anon", R"("Anon")", true, R"("Mine")"},
        {"pick", R"("b")", true, R"("Mine")"},
    }));
    CAPTURE(c.name, c.json);
    for (bool const rows : {false, true}) {
        CAPTURE(rows);
        auto const store = std::make_shared<GatedTableStateStore>(storeExecutor(), std::string{kStoredView});
        TableHarness h{store};
        auto screen = h.mount(rows ? rowsDocument() : modelDocument());
        if (!rows) {
            replyDescribe(h);
        }
        store->release();
        h.settle();
        REQUIRE(readJson(*screen, R"({"pref":"views"})") == R"("Mine")");
        auto const json = morph::ui::parseJson(c.json);
        REQUIRE(json.has_value());
        CHECK(screen->setPreference(std::string{c.name}, *morph::ui::valueFromJson(*json)) == c.accepted);
        h.advance(500ms);
        CHECK(readJson(*screen, R"({"pref":"views"})") == c.view);
        if (c.name == "views" && c.accepted) {
            REQUIRE(store->saves() == 1);
            auto const saved = table::readState(store->lastSaved());
            REQUIRE(saved.has_value());
            CHECK(saved->view == (c.json == "null" ? std::nullopt : std::optional<std::string>{"Other"}));
        } else {
            CHECK(store->saves() == 0);
        }
    }
}

TEST_CASE("covg table points: a gesture shows the layout at the points that name the table and no other",
          "[covg][ui][table][state]") {
    for (bool const rows : {false, true}) {
        CAPTURE(rows);
        auto const store = std::make_shared<GatedTableStateStore>(storeExecutor(), R"({"version":1})");
        TableHarness h{store};
        auto screen = h.mount(rows ? rowsDocument() : modelDocument());
        if (!rows) {
            replyDescribe(h);
        }
        store->release();
        h.settle();
        CHECK(readJson(*screen, R"({"pref":"cols"})") == "null");  // no layout held yet
        CHECK(readJson(*screen, R"({"pref":"views"})") == "null");
        chooseMenu(h, tableOf(h), 2, {"Hide column"});
        h.advance(500ms);
        CHECK(readJson(*screen, R"({"pref":"cols.hidden"})") != "[]");
        CHECK(readJson(*screen, R"({"pref":"views"})") == "null");  // no saved view
    }
}

TEST_CASE("covg table points: a refused save is said at a table over rows and a table over a model",
          "[covg][ui][table][state]") {
    for (bool const rows : {false, true}) {
        CAPTURE(rows);
        auto const store = std::make_shared<GatedTableStateStore>(storeExecutor(), R"({"version":1})");
        TableHarness h{store};
        auto screen = h.mount(rows ? rowsDocument() : modelDocument());
        if (!rows) {
            replyDescribe(h);
        }
        store->release();
        h.settle();
        store->refuseSaves("over the quota");
        chooseMenu(h, tableOf(h), 2, {"Hide column"});
        h.advance(500ms);
        CHECK(statusOf(h).message == "The table's arrangement could not be saved: over the quota");
    }
}

TEST_CASE("covg table state: a stored state that cannot be read is said and not used", "[covg][ui][table][state]") {
    for (bool const rows : {false, true}) {
        CAPTURE(rows);
        auto const store = std::make_shared<GatedTableStateStore>(storeExecutor(), "{not a state");
        TableHarness h{store};
        auto screen = h.mount(rows ? rowsDocument() : modelDocument());
        if (!rows) {
            replyDescribe(h);
        }
        store->release();
        h.settle();
        bool said = false;
        for (auto const& refusal : h.diagnostics) {
            said = said || refusal.reason.starts_with("the stored state of table `list` is not used:");
        }
        CHECK(said);
    }
}

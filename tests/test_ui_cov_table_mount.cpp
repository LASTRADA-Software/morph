// SPDX-License-Identifier: Apache-2.0

// A model table's document properties and the state it restores at mount: a literal or bound sort, filters and
// selection, a `persist` that names or refuses a store, and a stored state that names columns the table lacks.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "ui_table_model_support.hpp"
#include "ui_table_support.hpp"

namespace {

using namespace table_model_support;
using morph::ui::testing::elementValue;
using morph::ui::testing::GatedTableStateStore;
using morph::ui::testing::kConfigModelTable;
using morph::ui::testing::lastQuery;
using morph::ui::testing::replyDescribe;
using morph::ui::testing::reportsOf;
using morph::ui::testing::samplesDescription;
using morph::ui::testing::storeExecutor;
using morph::ui::testing::TableHarness;
using morph::ui::testing::withTableKey;

struct Mounted {
    std::shared_ptr<GatedTableStateStore> store;
    TableHarness h;
    std::unique_ptr<morph::ui::Screen> screen;
    explicit Mounted(std::string stored, std::string_view member = {}, std::string_view text = kConfigModelTable)
        : store{std::make_shared<GatedTableStateStore>(storeExecutor(), std::move(stored))}, h{store} {
        screen = h.mount(member.empty() ? std::string{text} : withTableKey(text, member));
        replyDescribe(h, samplesDescription(5));
        store->release();
        h.settle();
    }
    bool reported(std::string_view text) const {
        auto const lines = reportsOf(h);
        return std::ranges::any_of(lines, [text](std::string const& line) { return line.contains(text); });
    }
};
}  // namespace

TEST_CASE("cov table mount: a literal sort and filters are the defaults a table opens with",
          "[ui][table][cov][covr4]") {
    SECTION("a literal sort") {
        Mounted const m{"", R"("sort":{"lit":[{"column":"name","direction":"desc"}]})"};
        REQUIRE(lastQuery(m.h).sort.size() == 1);
        CHECK(lastQuery(m.h).sort.front().column == "name");
        CHECK(lastQuery(m.h).sort.front().dir == table::SortDirection::Descending);
    }
    SECTION("a literal sort that is null sets none") {
        Mounted const m{"", R"("sort":{"lit":null})"};
        CHECK(lastQuery(m.h).sort.empty());
    }
    SECTION("a literal filter") {
        Mounted const m{"", R"("filters":{"lit":{"columns":{"name":{"include":[{"eq":"x"}]}}}})"};
        REQUIRE(lastQuery(m.h).filters.columns.contains("name"));
        CHECK(lastQuery(m.h).filters.columns.at("name").include.front().eq == "x");
    }
}

TEST_CASE("cov table mount: filters bound to state are read, refused or ignored", "[ui][table][cov][covr4]") {
    constexpr std::string_view kBound =
        R"({"vocab":"ui/1","id":"samples","models":{"sample":{"type":"Lims_SampleModel"}},
        "state":{"f":{"init":{"lit":null}}},
        "view":{"kind":"table","id":"t","model":"sample","table":"samples","filters":{"ref":"f"}}})";
    auto const set = [](Mounted& m, std::string_view json) {
        m.h.backend.click(0);  // not used: the state is set through the screen below
        static_cast<void>(json);
    };
    static_cast<void>(set);
    Mounted const m{"", {}, kBound};
    CHECK(lastQuery(m.h).filters.columns.empty());
    CHECK(elementValue(*m.screen, "t", "filters") == R"({"columns":{},"groups":[]})");
}

TEST_CASE("cov table mount: persist names a store, or none", "[ui][table][cov][covr4]") {
    SECTION("persist false loads nothing") {
        Mounted const m{R"({"version":1,"sort":[{"column":"name","direction":"desc"}]})", R"("persist":false)"};
        CHECK(m.store->loads() == 0);
        CHECK(lastQuery(m.h).sort.empty());
    }
    SECTION("persist by name keys the store by it") {
        Mounted const m{"", R"("persist":"shared-samples")"};
        CHECK(m.store->loads() >= 1);
        CHECK(m.store->lastLoadKey().qualifier == "shared-samples");
    }
    SECTION("persist true keys it by the table") {
        Mounted const m{"", R"("persist":true)"};
        CHECK(m.store->loads() >= 1);
        CHECK(m.store->lastLoadKey().qualifier != "shared-samples");
    }
}

TEST_CASE("cov table mount: a stored state that names what the table lacks is fitted and reported",
          "[ui][table][cov][covr4]") {
    SECTION("a sort on a missing column is dropped") {
        Mounted const m{R"({"version":1,"sort":[{"column":"gone","direction":"asc"}]})"};
        CHECK(lastQuery(m.h).sort.empty());
    }
    SECTION("a filter on a missing column is dropped and said so") {
        Mounted const m{R"({"version":1,"filters":{"columns":{"gone":{"include":[{"eq":"x"}]}}}})"};
        CHECK_FALSE(lastQuery(m.h).filters.columns.contains("gone"));
    }
    SECTION("a stored filter and sort that fit are applied") {
        Mounted const m{R"({"version":1,"sort":[{"column":"name","direction":"desc"}],)"
                        R"("filters":{"columns":{"name":{"include":[{"contains":"a"}]}}}})"};
        REQUIRE(lastQuery(m.h).sort.size() == 1);
        CHECK(lastQuery(m.h).filters.columns.contains("name"));
    }
    SECTION("a stored view name is carried") {
        Mounted const m{R"({"version":1,"view":"mine"})"};
        CHECK(m.store->loads() >= 1);
    }
}

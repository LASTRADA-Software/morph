// SPDX-License-Identifier: Apache-2.0
//
// The column layout a table keeps: visibility, order, widths, pinning, user groups and display units, and how a
// stored layout is fitted to the declared columns.

#include <catch2/catch_test_macros.hpp>
#include <map>
#include <morph/table/engine.hpp>
#include <optional>
#include <string>
#include <vector>

using namespace morph::table;
using Ids = std::vector<std::string>;

TEST_CASE("a layout refuses what names no column or leaves no column to see", "[covc][table][layout]") {
    ColumnLayout layout{Ids{"a", "b", "c"}};
    CHECK_FALSE(layout.setVisible("zz", true));
    CHECK_FALSE(layout.setVisible("zz", false));
    CHECK(layout.setVisible("a", false));
    CHECK(layout.setVisible("b", false));
    CHECK_FALSE(layout.setVisible("c", false));
    CHECK(layout.visible() == Ids{"c"});
    CHECK(layout.setVisible("b", false));
    CHECK(layout.setVisible("a", true));
    CHECK(layout.visible() == Ids{"a", "c"});

    CHECK_FALSE(layout.move("zz", 0));
    CHECK(layout.move("c", 0));
    CHECK(layout.move("a", 99));
    CHECK(layout.order()[0] == "c");
    CHECK(layout.order()[2] == "a");

    CHECK_FALSE(layout.setWidth("zz", 5));
    CHECK_FALSE(layout.setWidth("a", 0));
    CHECK_FALSE(layout.setWidth("a", -3));
    CHECK(layout.setWidth("a", 7));
    CHECK(layout.widths().at("a") == 7);
    CHECK(layout.setWidth("a", std::nullopt));
    CHECK(layout.widths().empty());

    layout.setPinned(2);
    CHECK(layout.pinned() == 2);
    layout.setPinned(50);
    CHECK(layout.pinned() <= layout.visible().size());
}

TEST_CASE("a user group takes two to sixteen distinct columns that no group holds", "[covc][table][layout]") {
    Ids ids;
    for (int at = 0; at < 20; ++at) {
        ids.push_back("c" + std::to_string(at));
    }
    ColumnLayout layout{ids};
    auto const refused = [&](Ids columns) {
        auto const result = layout.formGroup("G", std::move(columns));
        REQUIRE_FALSE(result);
        CHECK(result.error().code == TableErrorCode::InvalidSpec);
        return result.error().message;
    };
    CHECK(refused({"c0"}).contains("2 to 16 columns, not 1"));
    CHECK(refused(ids).contains("not 20"));
    CHECK(refused({"c0", "nope"}) == "unknown column: nope");
    CHECK(refused({"c0", "c1", "c0"}) == "column c0 is named twice");

    auto const first = layout.formGroup("First", {"c0", "c1"});
    REQUIRE(first);
    CHECK(*first == "g1");
    CHECK(refused({"c1", "c2"}) == "column c1 is already in group First");
    auto const second = layout.formGroup("Second", {"c2", "c3"});
    REQUIRE(second);
    CHECK(*second == "g2");
    CHECK(layout.removeGroup("g1"));
    CHECK_FALSE(layout.removeGroup("g1"));
    auto const reused = layout.formGroup("Again", {"c0", "c1"});
    REQUIRE(reused);
    CHECK(*reused == "g1");
    CHECK(layout.groups().size() == 2);
}

TEST_CASE("a display unit is set per column and dimension, and cleared with its last dimension",
          "[covc][table][layout]") {
    ColumnLayout layout{Ids{"a", "b"}};
    CHECK_FALSE(layout.setUnit("zz", "mass", "kg"));
    CHECK_FALSE(layout.setUnit("a", "", "kg"));
    CHECK_FALSE(layout.setUnit("a", "mass", std::string{}));
    CHECK(layout.setUnit("a", "mass", "kg"));
    CHECK(layout.setUnit("a", "length", "m"));
    CHECK(layout.units().at("a").size() == 2);
    CHECK(layout.setUnit("a", "mass", std::nullopt));
    CHECK(layout.units().at("a").size() == 1);
    CHECK(layout.setUnit("a", "length", std::nullopt));
    CHECK(layout.units().empty());
    CHECK(layout.setUnit("b", "mass", std::nullopt));
    CHECK(layout.units().empty());
}

TEST_CASE("a stored layout is fitted to the columns now declared", "[covc][table][layout]") {
    ColumnLayout layout{Ids{"a", "b", "c", "d"}, Ids{"d", "gone"}};
    CHECK(layout.visible() == Ids{"a", "b", "c"});
    ColumnLayoutValue stored;
    stored.order = {"c", "gone", "a", "c"};
    stored.hidden = {"a", "gone"};
    stored.widths = {{"a", 4}, {"gone", 9}, {"c", -1}};
    layout.apply(stored);
    CHECK(layout.order()[0] == "c");
    CHECK(layout.order().size() == 4);
    auto const value = layout.value();
    CHECK(value.hidden.size() == 2);
    CHECK(value.widths == std::map<std::string, int>{{"a", 4}});

    ColumnLayoutValue allHidden;
    allHidden.hidden = {"a", "b", "c", "d"};
    layout.apply(allHidden);
    CHECK_FALSE(layout.visible().empty());

    layout.rebuild(Ids{"c", "z"});
    CHECK(layout.order().size() == 2);
    layout.reset();
    CHECK(Ids(layout.order().begin(), layout.order().end()) == Ids{"c", "z"});
    ColumnLayout const none{Ids{}};
    CHECK(none.order().empty());
}

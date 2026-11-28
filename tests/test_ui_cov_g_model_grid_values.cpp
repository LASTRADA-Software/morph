// SPDX-License-Identifier: Apache-2.0

// The draft an editor of a model table's grid opens with when the cell holds a value of the kind its field does not
// read: a day that is not a calendar day of the form, a number where a day is, a text where an instant is.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cstdint>
#include <morph/ui/grid.hpp>
#include <morph/ui/value.hpp>
#include <string>
#include <string_view>

#include "grid_test_support.hpp"
#include "ui_table_model_support.hpp"

namespace {

using namespace table_model_support;
using morph::test::grid::at;
using morph::test::grid::ModelGridFixture;
using morph::ui::Value;

struct Held {
    std::string_view name;
    std::string_view column;
    Value value;
    std::string_view draft;
};

}  // namespace

TEST_CASE("covg model grid values: an editor opens blank on a value its field does not read", "[covg][ui][grid]") {
    auto const& c = GENERATE(values<Held>({
        {"a day that is a number", "day", Value{std::int64_t{5}}, "1970-01-06"},
        {"a day with no month", "day", Value{std::string{"2026-xx-20"}}, ""},
        {"a day with no day", "day", Value{std::string{"2026-11-xx"}}, ""},
        {"a day with another separator", "day", Value{std::string{"2026/11/20"}}, ""},
        {"a day of the wrong length", "day", Value{std::string{"2026-11-2"}}, ""},
        {"a day whose year is digits only in part", "day", Value{std::string{"20xx-11-20"}}, ""},
        {"a day whose month is digits only in part", "day", Value{std::string{"2026-1x-20"}}, ""},
        {"a day whose year is signed", "day", Value{std::string{"-020-11-20"}}, ""},
        {"a day that reads", "day", Value{std::string{"2026-11-20"}}, "2026-11-20"},
        {"an instant that is a number", "at", Value{std::int64_t{5}}, "1970-01-01T00:00:05"},
        {"a decimal that is a text", "mass", Value{std::string{"heavy"}}, ""},
        {"a quantity that is a text", "weight", Value{std::string{"heavy"}}, ""},
        {"a date-time that is a text", "taken", Value{std::string{"soon"}}, ""},
    }));
    CAPTURE(c.name);
    ModelGridFixture f{{1}};
    morph::test::grid::setField(*f.rows, 1, std::string{c.column}, c.value);
    f.source->touch();
    f.grid->focusCell(at(1, std::string{c.column}));
    f.grid->press("F2");
    CHECK(f.source->draft(key(1), c.column) == c.draft);
    f.grid->press("Escape");
    CHECK(f.commits.sent.empty());
}

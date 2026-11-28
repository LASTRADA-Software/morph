// SPDX-License-Identifier: Apache-2.0

// A model table's grid says why a cell cannot be typed in: its row is not held, not loaded, locked, being saved, or
// its value is in a unit the field cannot hold; and it reads a row's key off a record of either kind of key.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cstdint>
#include <morph/ui/grid.hpp>
#include <morph/ui/value.hpp>
#include <optional>
#include <string>
#include <string_view>

#include "grid_test_support.hpp"
#include "ui_table_model_support.hpp"

namespace {

using namespace table_model_support;
using morph::test::grid::ModelGridFixture;
using morph::test::grid::setField;
using morph::ui::ModelGridSource;
using morph::ui::Value;

struct Facts {
    std::string_view name;
    std::int64_t key;
    std::string_view column;
};

}  // namespace

TEST_CASE("covg model grid: a row's key is read off a record of either kind of key", "[covg][ui][grid]") {
    ModelGridFixture f{{1}};
    auto const keyed = [&f](Value member) {
        morph::ui::ValueRecord record;
        if (!member.isNull()) {
            record.insert_or_assign("key", std::move(member));
        }
        return f.source->keyOf(Value{std::move(record)});
    };
    CHECK(keyed(Value{std::int64_t{7}}) == std::optional<morph::ui::Key>{morph::ui::Key{std::int64_t{7}}});
    CHECK(keyed(Value{std::string{"abc"}}) == std::optional<morph::ui::Key>{morph::ui::Key{std::string{"abc"}}});
    CHECK_FALSE(keyed(Value{true}).has_value());
    CHECK_FALSE(keyed(Value{}).has_value());
}

TEST_CASE("covg model grid: a cell says why it cannot be typed in", "[covg][ui][grid]") {
    ModelGridFixture f{{1, 2}};
    CHECK_FALSE(f.source->refusal(key(1), "label").has_value());
    CHECK(f.source->refusal(key(99), "label") == std::string{ModelGridSource::kUnloadedText});
    setField(*f.rows, 1, "lockedBy", Value{std::string{"Ann"}});
    f.source->touch();
    auto const locked = f.source->refusal(key(1), "label");
    REQUIRE(locked.has_value());
    CHECK(locked->contains("Ann"));
    CHECK_FALSE(f.source->refusal(key(2), "label").has_value());
    f.rows->rowStates.busyKeys(7, {row(2)});
    f.source->touch();
    CHECK(f.source->refusal(key(2), "label") == std::string{ModelGridSource::kBusyText});
    setField(*f.rows, 2, "weight",
             Value{morph::ui::Quantity{.value = morph::table::parseDecimal("2").value(), .unit = "oz"}});
    f.source->touch();
    f.rows->rowStates.settle(7);
    f.source->touch();
    CHECK(f.source->refusal(key(2), "weight") == std::string{ModelGridSource::kForeignUnitText});
}

TEST_CASE("covg model grid: a column of a level not loaded is read-only until it loads", "[covg][ui][grid]") {
    ModelGridFixture f{{1}};
    CHECK_FALSE(f.source->readOnly(key(1), "count"));
    f.rows->view.front().unloaded = {"counts"};
    f.source->touch();
    CHECK(f.source->readOnly(key(1), "count"));
    CHECK(f.source->refusal(key(1), "count") == std::string{ModelGridSource::kUnloadedText});
    CHECK_FALSE(f.source->readOnly(key(1), "label"));
}

TEST_CASE("covg model grid: an action the user may not run leaves its column read-only", "[covg][ui][grid]") {
    ModelGridFixture f{{1}};
    CHECK_FALSE(f.source->readOnly(key(1), "label"));
    f.permitted.clear();
    f.source->touch();
    CHECK(f.source->readOnly(key(1), "label"));
    CHECK(f.source->readOnly(key(1), "note"));  // no edit declared at all
}

TEST_CASE("covg model grid: a frozen companion makes its cell read-only", "[covg][ui][grid]") {
    ModelGridFixture f{{1, 2}};
    setField(*f.rows, 1, "countFrozen", Value{true});
    f.source->touch();
    CHECK(f.source->readOnly(key(1), "count"));
    CHECK_FALSE(f.source->readOnly(key(2), "count"));
}

// SPDX-License-Identifier: Apache-2.0
//
// A form session's options shared between rows, its reverts, its refills and what a refusal of its input leaves
// untouched.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <morph/forms/engine/form_session.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "forms_engine_support.hpp"

namespace {

using morph::test::formsengine::Harness;

constexpr std::string_view kLines = R"({"properties":{
  "lines":{"type":"array","x-order":0,"items":{"type":"object","properties":{
    "material":{"type":["integer","null"],"x-order":0,"x-optionsAction":"ListMaterials","x-optionValue":"id",
                "x-optionLabel":"name"},
    "qty":{"type":"integer","x-order":1}}}}},
  "required":[]})";

constexpr std::string_view kMaterials = R"([{"id":1,"name":"Steel"},{"id":2,"name":"Brass"}])";

std::vector<std::string> labels(morph::forms::FieldState const& field) {
    std::vector<std::string> out;
    for (auto const& option : field.options()) {
        out.push_back(option.label);
    }
    return out;
}

morph::forms::FieldState& material(morph::forms::FormSession& form, std::uint64_t row) {
    for (auto const& state : form.field("lines").rows()) {
        if (state->key() == row) {
            return *state->field("material");
        }
    }
    throw std::out_of_range{"no such row"};
}

}  // namespace

TEST_CASE("rows that open the same options share one fetch, and a row opened later takes the reply at once",
          "[covf][forms][session]") {
    Harness harness;
    harness.server.optionsWith([](auto const&) { return std::nullopt; });
    auto const form = harness.session("Lines", kLines);
    auto const first = form->field("lines").addRow();
    auto const second = form->field("lines").addRow();
    harness.settle();
    CHECK(harness.server.fetches().empty());
    material(*form, first).openOptions();
    material(*form, second).openOptions();
    harness.settle();
    REQUIRE(harness.server.fetches().size() == 1);
    CHECK(material(*form, first).optionsPending());
    CHECK(material(*form, second).optionsPending());
    harness.server.resolveFetch(0, std::string{kMaterials});
    harness.settle();
    CHECK(labels(material(*form, first)) == std::vector<std::string>{"Steel", "Brass"});
    CHECK(labels(material(*form, second)) == std::vector<std::string>{"Steel", "Brass"});

    auto const third = form->field("lines").addRow();
    material(*form, third).openOptions();
    harness.settle();
    CHECK(harness.server.fetches().size() == 1);
    CHECK(labels(material(*form, third)) == std::vector<std::string>{"Steel", "Brass"});
}

TEST_CASE("a shared options fetch that failed is forgotten, so opening a row's select asks again",
          "[covf][forms][session]") {
    Harness harness;
    harness.server.optionsWith([](auto const&) { return std::nullopt; });
    auto const form = harness.session("Lines", kLines);
    auto const first = form->field("lines").addRow();
    auto const second = form->field("lines").addRow();
    material(*form, first).openOptions();
    material(*form, second).openOptions();
    harness.settle();
    REQUIRE(harness.server.fetches().size() == 1);
    harness.server.rejectFetch(0, "the materials are unavailable");
    harness.settle();
    CHECK_FALSE(material(*form, first).optionsPending());
    CHECK_FALSE(material(*form, second).optionsPending());
    CHECK(material(*form, first).options().empty());

    harness.server.optionsWith([](auto const&) { return std::optional<std::string>{std::string{kMaterials}}; });
    material(*form, first).retryOptions();
    harness.settle();
    REQUIRE(harness.server.fetches().size() == 2);
    CHECK(labels(material(*form, first)) == std::vector<std::string>{"Steel", "Brass"});
    // A select that is already answered fetches nothing.
    material(*form, first).retryOptions();
    harness.settle();
    CHECK(harness.server.fetches().size() == 2);
}

namespace {

constexpr std::string_view kNested = R"({"properties":{
  "name":{"type":"string","x-order":0},
  "crate":{"type":"object","x-order":1,"properties":{
    "width":{"type":"integer","x-order":0},
    "label":{"type":"string","x-order":1}}},
  "boxes":{"type":"array","x-order":2,"items":{"type":"object","properties":{
    "count":{"type":"integer","x-order":0}}}}},
  "required":[]})";

}  // namespace

TEST_CASE("reverting an object restores each member, and a revert of what is not there changes nothing",
          "[covf][forms][session]") {
    Harness harness;
    auto const form = harness.session("Nested", kNested);
    form->prefill(R"({"name":"n","crate":{"width":4,"label":"L"},"boxes":[{"count":1}]})");
    harness.settle();
    harness.type(*form, "crate.width", "9");
    harness.type(*form, "crate.label", "changed");
    CHECK(form->modified("crate.width"));
    CHECK_FALSE(form->modified("crate"));
    form->revert("crate");
    harness.settle();
    CHECK(form->field("crate.width").draft() == "4");
    CHECK(form->field("crate.label").draft() == "L");
    CHECK_FALSE(form->modified("crate.width"));

    form->revert("no.such.field");
    form->revert("boxes#999");
    form->revert("boxes");
    CHECK(form->field("name").draft() == "n");
    CHECK_FALSE(form->modified("no.such.field"));
    CHECK_FALSE(form->modified("boxes"));
    CHECK_FALSE(form->rowModified("nothing", 1));
    CHECK_FALSE(form->rowModified("name", 1));
    CHECK_FALSE(form->rowModified("boxes", 999));
}

TEST_CASE("a row added since the load is modified, and reverting it removes it", "[covf][forms][session]") {
    Harness harness;
    auto const form = harness.session("Nested", kNested);
    form->prefill(R"({"boxes":[{"count":1}]})");
    harness.settle();
    auto const loaded = form->field("boxes").rows().front()->key();
    CHECK_FALSE(form->rowModified("boxes", loaded));
    auto const added = form->field("boxes").addRow();
    harness.settle();
    CHECK(form->rowModified("boxes", added));
    harness.type(*form, "boxes#" + std::to_string(loaded) + ".count", "7");
    CHECK(form->rowModified("boxes", loaded));
    form->revert("boxes#" + std::to_string(loaded));
    harness.settle();
    CHECK(form->field("boxes#" + std::to_string(loaded) + ".count").draft() == "1");
    form->revert("boxes#" + std::to_string(added));
    harness.settle();
    CHECK(form->field("boxes").rows().size() == 1);
}

TEST_CASE("text that is not a JSON object loads nothing, and assigning one value leaves the rest",
          "[covf][forms][session]") {
    Harness harness;
    auto const form = harness.session("Nested", kNested);
    form->prefill(R"({"name":"n","crate":{"width":4}})");
    harness.settle();
    for (std::string_view const bad : {"not json", "[1,2]", "7", ""}) {
        CAPTURE(bad);
        form->prefill(bad);
        form->refill(bad);
        harness.settle();
        CHECK(form->field("name").draft() == "n");
        CHECK(form->field("crate.width").draft() == "4");
    }
    form->assign("name", "not json");
    CHECK(form->field("name").draft() == "n");
    form->assign("crate.width", "11");
    harness.settle();
    CHECK(form->field("crate.width").draft() == "11");
    CHECK_THROWS_AS(form->assign("crate.nothing", "1"), std::out_of_range);
}

TEST_CASE("a refill leaves the entry in another unit alone when the record holds the same amount",
          "[covf][forms][session]") {
    constexpr std::string_view kMass = R"({"properties":{
      "mass":{"type":["object","null"],"title":"Mass","x-decimalPlaces":3,
              "ExtUnits":{"unitAscii":"kg","unitUnicode":"kg"},
              "x-unitAlternatives":[{"id":"g","display":"g","decimals":1,"num":1,"den":1000}],"x-order":0}},
      "required":[]})";
    morph::forms::FieldOverlays overlays;
    overlays.emplace("mass", morph::forms::FieldOverlay{.unit = "g"});
    auto model = morph::forms::FormModel::fromSchema("Mass", kMass, overlays);
    REQUIRE(model.has_value());
    Harness harness;
    morph::forms::FormSession form{harness.rt, *std::move(model), harness.server.submitter(),
                                   harness.server.fetcher()};
    form.prefill(R"({"mass":{"num":2000,"den":1000,"dp":3}})");
    harness.settle();
    CHECK(form.field("mass").draft() == "2000.0");
    auto const loads = form.loadGeneration();
    // The record's own amount, in the canonical unit, is what the entry already says in grams.
    form.refill(R"({"mass":{"num":2000,"den":1000,"dp":3}})");
    harness.settle();
    CHECK(form.loadGeneration() == loads);
    CHECK(form.field("mass").unit().peek() == 1);
    // A different amount is a new value, shown in the entry unit.
    form.refill(R"({"mass":{"num":3000,"den":1000,"dp":3}})");
    harness.settle();
    CHECK(form.field("mass").draft() == "3000.0");
    CHECK(form.loadGeneration() > loads);
    // A record without the amount blanks the entry.
    form.refill(R"({})");
    harness.settle();
    CHECK(form.field("mass").draft().empty());
}

namespace {

constexpr std::string_view kDeepForm = R"({"type":"object","properties":{
  "l1":{"type":"object","x-order":0,"properties":{"l2":{"type":"object","properties":{"l3":{"type":"object","properties":{
    "l4":{"type":"object","properties":{
      "obj":{"type":"object","properties":{"x":{"type":"integer"}}},
      "list":{"type":"array","items":{"type":"object","properties":{"y":{"type":"integer"}}}}}}}}}}}},
  "top":{"type":"integer","x-order":1}},
  "required":["top"]})";

constexpr std::string_view kDeepList = "l1.l2.l3.l4.list";
constexpr std::string_view kDeepObject = "l1.l2.l3.l4.obj";

}  // namespace

TEST_CASE("a field below the nesting depth is never modified, reverted or reported as the loaded value",
          "[covf][forms][session]") {
    Harness harness;
    auto const form = harness.session("Deep", kDeepForm);
    form->prefill(R"({"top":1,"l1":{"l2":{"l3":{"l4":{"obj":{"x":7},"list":[{"y":1}]}}}}})");
    harness.settle();
    CHECK_FALSE(form->modified(kDeepList));
    CHECK_FALSE(form->modified(kDeepObject));
    harness.type(*form, kDeepList, R"([{"y":5}])");
    CHECK_FALSE(form->modified(kDeepList));
    form->revert(kDeepList);
    form->revert(kDeepObject);
    harness.settle();
    CHECK(form->field(kDeepList).draft() == R"([{"y":5}])");
    // A list below the depth takes no rows of its own.
    CHECK(form->field(kDeepList).addRow() == 0);
    CHECK(form->field("top").addRow() == 0);
}

TEST_CASE(
    "a field below the nesting depth is sent as typed when it is JSON, and as nothing or an empty list when blank",
    "[covf][forms][session]") {
    Harness harness;
    auto const form = harness.session("Deep", kDeepForm);
    harness.type(*form, "top", "1");
    REQUIRE(form->body().has_value());
    CHECK(*form->body() == R"({"top":1})");
    harness.type(*form, std::string{kDeepList}, "[]");
    CHECK(*form->body() == R"({"l1":{"l2":{"l3":{"l4":{"list":[]}}}},"top":1})");
    harness.type(*form, std::string{kDeepList}, "");
    CHECK(*form->body() == R"({"top":1})");
    harness.type(*form, std::string{kDeepList}, R"([{"y":2}])");
    // Only a blank list or `[]` can be sent from below the depth: any other text is not one the form can encode.
    CHECK_FALSE(form->body().has_value());
    CHECK(form->field(kDeepList).error().get().has_value());
    harness.type(*form, std::string{kDeepList}, "");
    CHECK(*form->body() == R"({"top":1})");
}

TEST_CASE("a refill replaces an object, a collection and a field below the depth a person has not touched",
          "[covf][forms][session]") {
    Harness harness;
    auto const form = harness.session("Nested", kNested);
    form->prefill(R"({"name":"n","crate":{"width":4,"label":"L"},"boxes":[{"count":1}]})");
    harness.settle();
    form->refill(R"({"name":"n","crate":{"width":8,"label":"M"},"boxes":[{"count":1},{"count":2}]})");
    harness.settle();
    CHECK(form->field("crate.width").draft() == "8");
    CHECK(form->field("crate.label").draft() == "M");
    CHECK(form->field("boxes").rows().size() == 2);

    Harness deepHarness;
    auto const deep = deepHarness.session("Deep", kDeepForm);
    deep->prefill(R"({"top":1,"l1":{"l2":{"l3":{"l4":{"obj":{"x":7}}}}}})");
    deepHarness.settle();
    deep->refill(R"({"top":2,"l1":{"l2":{"l3":{"l4":{"obj":{"x":9}}}}}})");
    deepHarness.settle();
    CHECK(deep->field("top").draft() == "2");
    CHECK(deep->field(kDeepObject).draft().contains(R"("x":9)"));
}

TEST_CASE("a string field that may be sent empty is engaged by a string value and not by null",
          "[covf][forms][session]") {
    constexpr std::string_view kBlank = R"({"properties":{
      "remark":{"type":["string","null"],"x-order":0,"x-blankAs":"empty"},
      "tag":{"type":["string","null"],"x-order":1}},"required":[]})";
    Harness harness;
    auto const form = harness.session("Blank", kBlank);
    form->prefill(R"({"remark":null})");
    harness.settle();
    CHECK_FALSE(form->field("remark").engaged().peek());
    CHECK(form->body() == std::optional<std::string>{"{}"});
    form->prefill(R"({"remark":""})");
    harness.settle();
    CHECK(form->field("remark").engaged().peek());
    CHECK(form->body() == std::optional<std::string>{R"({"remark":""})"});
    form->prefill(R"({"remark":3})");
    harness.settle();
    CHECK_FALSE(form->field("remark").engaged().peek());
    form->prefill(R"({"tag":"t"})");
    harness.settle();
    CHECK_FALSE(form->field("remark").engaged().peek());
    CHECK(form->body() == std::optional<std::string>{R"({"tag":"t"})"});
}

TEST_CASE("assigning a member does not count the fields whose names it begins with", "[covf][forms][session]") {
    constexpr std::string_view kNames = R"({"properties":{
      "a":{"type":"string","x-order":0},
      "ab":{"type":"string","x-order":1}},"required":[],"x-submitMode":"explicit"})";
    Harness harness;
    auto const form = harness.session("Names", kNames);
    form->prefill(R"({"a":"1","ab":"2"})");
    harness.settle();
    harness.type(*form, "a", "typed");
    form->assign("ab", R"("assigned")");
    harness.settle();
    CHECK(form->field("ab").draft() == "assigned");
    CHECK(form->modified("a"));
    form->refill(R"({"a":"record","ab":"record"})");
    harness.settle();
    // `a` was edited and is kept; `ab` was assigned, which counts as loaded, so the record replaces it.
    CHECK(form->field("a").draft() == "typed");
    CHECK(form->field("ab").draft() == "record");
}

TEST_CASE("a member of a read-only object is read-only, and a refusal is held only for a field that was refused",
          "[covf][forms][session]") {
    constexpr std::string_view kLocked = R"({"properties":{
      "frozen":{"type":"object","x-order":0,"x-readonly":true,"properties":{"v":{"type":"integer","x-order":0}}},
      "open":{"type":"integer","x-order":1}},"required":[]})";
    Harness harness;
    auto const form = harness.session("Locked", kLocked);
    CHECK(form->field("frozen.v").readonly().get());
    CHECK_FALSE(form->field("open").readonly().get());
    CHECK_FALSE(form->refusal("open").has_value());
    CHECK_FALSE(form->refusal("no.such.field").has_value());
}

TEST_CASE("the implicit group of a form without a layout has no title", "[covf][forms][session]") {
    Harness harness;
    auto const form = harness.session("Nested", kNested);
    CHECK(form->groupTitle(0).empty());
}

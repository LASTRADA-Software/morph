// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <morph/forms/engine/form_session.hpp>
#include <string>
#include <string_view>

#include "forms_engine_support.hpp"

namespace {

using morph::forms::FormSessionOptions;
using morph::test::formsengine::Harness;

constexpr std::string_view kIgnition =
    R"({"type":"object","$defs":{"double":{"type":"number"},"int32_t":{"type":"integer","minimum":-2147483648,"maximum":2147483647}},
  "properties":{
  "method":{"type":"string","oneOf":[{"title":"Furnace","const":"Furnace"},{"title":"Infrared","const":"Infrared"}],"x-order":0},
  "specimen":{"type":"object","x-order":1,"required":["massOfContainer","massOfContainerAndSampleBeforeIgnition","testTemperature"],"properties":{
    "massOfContainer":{"$ref":"#/$defs/double","x-order":0,"ExtUnits":{"unitAscii":"g","unitUnicode":"g"},"x-displayDecimals":1},
    "massOfContainerAndSampleBeforeIgnition":{"$ref":"#/$defs/double","x-order":1,"ExtUnits":{"unitAscii":"g","unitUnicode":"g"},"x-displayDecimals":1},
    "readoutBinderContent":{"anyOf":[{"$ref":"#/$defs/double"},{"type":"null"}],"x-order":2,"ExtUnits":{"unitAscii":"pct","unitUnicode":"%"},"x-displayDecimals":2},
    "testTemperature":{"$ref":"#/$defs/int32_t","x-order":3}}},
  "calibrationFactor":{"$ref":"#/$defs/double","x-order":2},
  "driedSample":{"type":"boolean","x-order":3},
  "testedByNr":{"anyOf":[{"$ref":"#/$defs/int32_t"},{"type":"null"}],"x-order":4}},
  "required":["method","specimen","calibrationFactor","driedSample"]})";

constexpr std::string_view kDensity = R"({"type":"object","$defs":{"double":{"type":"number"},
  "lab::PycnometerDetermination":{"type":"object","required":["massPycnometerAndSample","excluded"],"properties":{
    "massPycnometerEmpty":{"anyOf":[{"$ref":"#/$defs/double"},{"type":"null"}],"x-order":0,"ExtUnits":{"unitAscii":"g","unitUnicode":"g"},"x-displayDecimals":2},
    "massPycnometerAndSample":{"$ref":"#/$defs/double","x-order":1,"ExtUnits":{"unitAscii":"g","unitUnicode":"g"},"x-displayDecimals":2},
    "excluded":{"type":"boolean","x-order":2},
    "computedDensity":{"anyOf":[{"$ref":"#/$defs/double"},{"type":"null"}],"x-order":3,"x-readonly":true}}}},
  "properties":{"data":{"type":"object","required":["useSpecificGravity"],"x-order":0,"properties":{
    "useSpecificGravity":{"type":"boolean","x-order":0},
    "testLiquidTemperature":{"anyOf":[{"$ref":"#/$defs/double"},{"type":"null"}],"x-order":1,"x-displayDecimals":1},
    "testLiquidName":{"type":["string","null"],"x-order":2},
    "determination1":{"anyOf":[{"$ref":"#/$defs/lab::PycnometerDetermination"},{"type":"null"}],"x-order":3},
    "determination2":{"anyOf":[{"$ref":"#/$defs/lab::PycnometerDetermination"},{"type":"null"}],"x-order":4}}}},
  "required":["data"]})";

constexpr std::string_view kGrading = R"({"$defs":{"double":{"type":"number"},"int64_t":{"type":"integer"},
  "Row":{"type":"object","required":["sieve","passing"],"properties":{
    "note":{"type":["string","null"],"x-order":2},
    "sieve":{"$ref":"#/$defs/double","x-order":0,"ExtUnits":{"unitAscii":"mm","unitUnicode":"mm"}},
    "passing":{"type":["object","null"],"properties":{"num":{"type":"integer"},"den":{"type":"integer"},"dp":{"type":"integer"}},"ExtUnits":{"unitAscii":"pct","unitUnicode":"%"},"x-decimalPlaces":1,"x-order":1},
    "order":{"$ref":"#/$defs/int64_t","x-order":3,"x-readonly":true}}}},
  "properties":{"rows":{"type":"array","items":{"$ref":"#/$defs/Row"},"x-order":0},
    "tags":{"type":"array","items":{"type":"string"},"x-order":1}},"required":["rows"]})";

void fillIgnitionScalars(Harness& harness, morph::forms::FormSession& form) {
    harness.type(form, "method", R"("Furnace")");
    harness.type(form, "calibrationFactor", "0.25");
    harness.type(form, "driedSample", "true");
}

}  // namespace

TEST_CASE("nested: an object encodes member by member with each member's own encoder", "[forms-engine][nested]") {
    Harness harness;
    auto const form = harness.session("T_Ignition", kIgnition);
    fillIgnitionScalars(harness, *form);
    CHECK_FALSE(form->ready());
    harness.type(*form, "specimen.massOfContainer", "512.3");
    harness.type(*form, "specimen.massOfContainerAndSampleBeforeIgnition", "2012.8");
    harness.type(*form, "specimen.readoutBinderContent", "5.25");
    harness.type(*form, "specimen.testTemperature", "538");
    REQUIRE(form->ready());
    CHECK(harness.server.lastBody() ==
          R"({"method":"Furnace","specimen":{"massOfContainer":512.3,"massOfContainerAndSampleBeforeIgnition":2012.8,)"
          R"("readoutBinderContent":5.25,"testTemperature":538},"calibrationFactor":0.25,"driedSample":true})");
    CHECK(form->field("specimen.massOfContainer").path() == "specimen.massOfContainer");
}

TEST_CASE("nested: a member that does not encode, or a blank required member, keeps the form unready",
          "[forms-engine][nested]") {
    Harness harness;
    auto const form = harness.session("T_Ignition", kIgnition);
    fillIgnitionScalars(harness, *form);
    harness.type(*form, "specimen.massOfContainerAndSampleBeforeIgnition", "2.0");
    harness.type(*form, "specimen.testTemperature", "538");
    CHECK_FALSE(form->ready());  // massOfContainer is required and blank
    CHECK(form->field("specimen").error().get()->code == "incomplete");
    harness.type(*form, "specimen.massOfContainer", "1.25");
    CHECK_FALSE(form->ready());  // finer than x-displayDecimals
    harness.type(*form, "specimen.massOfContainer", "abc");
    CHECK_FALSE(form->ready());
    harness.type(*form, "specimen.massOfContainer", "1.2");
    harness.type(*form, "specimen.testTemperature", "1.5");
    CHECK_FALSE(form->ready());
    harness.type(*form, "specimen.testTemperature", "538");
    CHECK(form->ready());
    CHECK(harness.server.submits().size() == 1);
}

TEST_CASE("nested: members read in the display locale", "[forms-engine][nested]") {
    Harness harness;
    auto const form = harness.session("T_Ignition", kIgnition,
                                      FormSessionOptions{.locale = {.decimalSeparator = ",", .groupSeparator = "."}});
    harness.type(*form, "method", R"("Furnace")");
    harness.type(*form, "calibrationFactor", "0,25");
    harness.type(*form, "driedSample", "true");
    harness.type(*form, "specimen.massOfContainer", "1.512,3");
    harness.type(*form, "specimen.massOfContainerAndSampleBeforeIgnition", "2012,8");
    harness.type(*form, "specimen.readoutBinderContent", "5,25");
    harness.type(*form, "specimen.testTemperature", "538");
    CHECK(
        form->body() ==
        R"({"method":"Furnace","specimen":{"massOfContainer":1512.3,"massOfContainerAndSampleBeforeIgnition":2012.8,)"
        R"("readoutBinderContent":5.25,"testTemperature":538},"calibrationFactor":0.25,"driedSample":true})");
}

TEST_CASE("nested: two levels deep, blank optional leaves and objects are left out", "[forms-engine][nested]") {
    Harness harness;
    auto const form = harness.session("T_Density", kDensity);
    CHECK_FALSE(form->ready());  // data is required and blank
    harness.type(*form, "data.useSpecificGravity", "false");
    harness.type(*form, "data.testLiquidTemperature", "25.0");
    harness.type(*form, "data.testLiquidName", "water");
    harness.type(*form, "data.determination1.massPycnometerEmpty", "1450.10");
    harness.type(*form, "data.determination1.massPycnometerAndSample", "3450.25");
    harness.type(*form, "data.determination1.excluded", "false");
    CHECK(form->body() ==
          R"({"data":{"useSpecificGravity":false,"testLiquidTemperature":25.0,"testLiquidName":"water",)"
          R"("determination1":{"massPycnometerEmpty":1450.10,"massPycnometerAndSample":3450.25,"excluded":false}}})");
    (*form).reset();
    harness.settle();
    harness.type(*form, "data.useSpecificGravity", "true");
    harness.type(*form, "data.testLiquidName", "  ");
    CHECK(form->body() == R"({"data":{"useSpecificGravity":true}})");
    harness.type(*form, "data.determination2.massPycnometerEmpty", "10.00");
    CHECK_FALSE(form->ready());  // a partly filled optional object needs its required members
    harness.type(*form, "data.determination2.massPycnometerAndSample", "20.00");
    harness.type(*form, "data.determination2.excluded", "true");
    CHECK(form->body() == R"({"data":{"useSpecificGravity":true,"determination2":{"massPycnometerEmpty":10.00,)"
                          R"("massPycnometerAndSample":20.00,"excluded":true}}})");
}

TEST_CASE("nested: a required boolean member of a filled object answers false", "[forms-engine][nested]") {
    Harness harness;
    auto const form = harness.session("T_Density", kDensity);
    harness.type(*form, "data.testLiquidName", "water");
    CHECK(form->body() == R"({"data":{"useSpecificGravity":false,"testLiquidName":"water"}})");
}

TEST_CASE("nested: a prefill decodes into the members and round-trips", "[forms-engine][nested]") {
    constexpr std::string_view kStored =
        R"({"data":{"useSpecificGravity":false,"testLiquidTemperature":25.5,"determination1":{"massPycnometerEmpty":1450.1,)"
        R"("massPycnometerAndSample":3450.25,"excluded":true,"computedDensity":2.412}}})";
    Harness harness;
    auto const form = harness.session("T_Density", kDensity);
    form->prefill(kStored);
    harness.settle();
    CHECK(harness.server.submits().empty());
    CHECK(form->field("data.useSpecificGravity").text().peek() == "false");
    CHECK(form->field("data.testLiquidTemperature").text().peek() == "25.5");
    CHECK(form->field("data.determination1.massPycnometerEmpty").text().peek() == "1450.10");
    CHECK(form->field("data.determination1.excluded").text().peek() == "true");
    CHECK(form->field("data.determination2").blank());
    CHECK(
        form->body() ==
        R"({"data":{"useSpecificGravity":false,"testLiquidTemperature":25.5,"determination1":{"massPycnometerEmpty":1450.10,)"
        R"("massPycnometerAndSample":3450.25,"excluded":true,"computedDensity":2.412}}})");
    (*form).reset();
    harness.settle();
    CHECK(form->field("data").blank());
    CHECK_FALSE(form->ready());

    Harness german;
    auto const germanForm = german.session(
        "T_Ignition", kIgnition, FormSessionOptions{.locale = {.decimalSeparator = ",", .groupSeparator = "."}});
    germanForm->prefill(
        R"({"method":"Infrared","calibrationFactor":0.5,"driedSample":false,"specimen":{"massOfContainer":512.3,)"
        R"("massOfContainerAndSampleBeforeIgnition":2012.8,"testTemperature":538}})");
    german.settle();
    CHECK(germanForm->field("specimen.massOfContainer").text().peek() == "512,3");
    CHECK(germanForm->field("specimen.readoutBinderContent").blank());
    CHECK(germanForm->body() ==
          R"({"method":"Infrared","specimen":{"massOfContainer":512.3,)"
          R"("massOfContainerAndSampleBeforeIgnition":2012.8,"testTemperature":538},"calibrationFactor":0.5,)"
          R"("driedSample":false})");
}

TEST_CASE("nested: collection rows encode with each member's encoder; an empty required collection is []",
          "[forms-engine][nested]") {
    Harness harness;
    auto const form = harness.session("T_Grading", kGrading);
    CHECK(form->body() == R"({"rows":[]})");
    auto& rows = form->field("rows");
    rows.addRow();
    harness.type(*form, "rows[0].sieve", "31.5");
    CHECK_FALSE(form->ready());  // passing is required and blank
    harness.type(*form, "rows[0].passing", "100.0");
    auto const second = rows.addRow();
    harness.type(*form, "rows[1].sieve", "0.063");
    harness.type(*form, "rows[1].passing", "4.2");
    harness.type(*form, "rows[1].note", "fines");
    CHECK(form->body() == R"({"rows":[{"sieve":31.5,"passing":{"num":1000,"den":10,"dp":1}},)"
                          R"({"sieve":0.063,"passing":{"num":42,"den":10,"dp":1},"note":"fines"}]})");
    harness.type(*form, "rows[1].passing", "1.25");
    CHECK_FALSE(form->ready());  // an over-precise Quantity cell is refused, not rounded
    harness.type(*form, "rows[1].passing", "1.2");
    CHECK(form->ready());
    rows.removeRow(rows.rows().front()->key());
    harness.settle();
    REQUIRE(rows.rows().size() == 1);
    CHECK(rows.rows().front()->key() == second);
    CHECK(form->field("rows[0].note").text().peek() == "fines");
    CHECK(form->field("rows[0].sieve").path() == "rows#" + std::to_string(second) + ".sieve");
}

TEST_CASE("nested: loaded cells read as their text; a reset empties the rows", "[forms-engine][nested]") {
    Harness harness;
    auto const form = harness.session("T_Grading", kGrading);
    form->prefill(R"({"rows":[{"sieve":2.5,"passing":{"num":30,"den":10,"dp":1},"order":7}]})");
    harness.settle();
    CHECK(form->body() == R"({"rows":[{"sieve":2.5,"passing":{"num":30,"den":10,"dp":1},"order":7}]})");
    CHECK(form->field("rows[0].passing").text().peek() == "3.0");
    (*form).reset();
    harness.settle();
    CHECK(form->field("rows").rows().empty());
    CHECK(form->body() == R"({"rows":[]})");
}

TEST_CASE("nested: rows inside an object reuse the row encoder", "[forms-engine][nested]") {
    Harness harness;
    auto const form =
        harness.session("T_Grading", R"({"$defs":{"Row":{"type":"object","required":["sieve","passing"],"properties":{
        "sieve":{"type":"number","x-order":0},
        "passing":{"type":["object","null"],"ExtUnits":{"unitAscii":"pct","unitUnicode":"%"},"x-decimalPlaces":1,"x-order":1}}}},
      "properties":{"grading":{"type":"object","x-order":0,"required":["label","rows"],"properties":{
        "label":{"type":"string","x-order":0},"rows":{"type":"array","items":{"$ref":"#/$defs/Row"},"x-order":1}}}},
      "required":["grading"]})");
    harness.type(*form, "grading.label", "A");
    CHECK(form->body() == R"({"grading":{"label":"A","rows":[]}})");
    form->field("grading.rows").addRow();
    harness.type(*form, "grading.rows[0].sieve", "8");
    CHECK_FALSE(form->ready());
    harness.type(*form, "grading.rows[0].passing", "55.0");
    CHECK(form->body() == R"({"grading":{"label":"A","rows":[{"sieve":8,"passing":{"num":550,"den":10,"dp":1}}]}})");
}

TEST_CASE("nested: an acyclic aggregate is drawn and encoded; an optional one may be left out",
          "[forms-engine][nested]") {
    Harness harness;
    auto const acyclic = harness.session("AcyclicAction", R"({"properties":{"id":{"type":"integer","x-order":0},
      "address":{"type":"object","x-order":1,"required":["street","city"],"properties":{
        "street":{"type":"string","x-order":0},"city":{"type":"string","x-order":1}}}},"required":["id","address"]})");
    harness.type(*acyclic, "id", "7");
    harness.type(*acyclic, "address.street", "Main");
    CHECK_FALSE(acyclic->ready());
    harness.type(*acyclic, "address.city", "Springfield");
    CHECK(acyclic->body() == R"({"id":7,"address":{"street":"Main","city":"Springfield"}})");
    auto const optional =
        harness.session("OptionalNestedAction", R"({"properties":{"id":{"type":"integer","x-order":0},
      "note":{"type":"string","x-order":1},"address":{"type":"object","x-order":2,"required":["street"],
      "properties":{"street":{"type":"string","x-order":0}}}},"required":["id"]})");
    harness.type(*optional, "id", "7");
    CHECK(optional->body() == R"({"id":7})");
}

TEST_CASE("nested: a self-referential type stops where it repeats, and a value loaded there is unrepresentable",
          "[forms-engine][nested]") {
    Harness harness;
    auto const form =
        harness.session("T_Tree", R"({"$defs":{"TreeNode":{"type":"object","required":["name"],"properties":{
        "name":{"type":"string","x-order":0},"child":{"anyOf":[{"$ref":"#/$defs/TreeNode"},{"type":"null"}],"x-order":1}}}},
      "properties":{"root":{"$ref":"#/$defs/TreeNode","x-order":0}},"required":["root"]})");
    REQUIRE(form->field("root").members().size() == 2);
    CHECK(form->field("root.child").spec().truncated);
    harness.type(*form, "root.name", "top");
    CHECK(harness.server.lastBody() == R"({"root":{"name":"top"}})");
    form->prefill(R"({"root":{"name":"top","child":{"name":"leaf"}}})");
    harness.settle();
    CHECK_FALSE(form->ready());
    CHECK(form->field("root.child").error().get()->code == "unrepresentable");

    auto const tree = harness.session(
        "SelfAction", R"({"$defs":{"probe::TreeNode":{"type":"object","required":["name","children"],"properties":{
        "children":{"type":"array","items":{"$ref":"#/$defs/probe::TreeNode"},"x-order":1},"name":{"type":"string","x-order":0}}}},
      "properties":{"id":{"type":"integer","x-order":0},"root":{"$ref":"#/$defs/probe::TreeNode","x-order":1}},"required":["id","root"]})");
    harness.type(*tree, "id", "7");
    harness.type(*tree, "root.name", "top");
    CHECK(tree->field("root.children").spec().truncated);  // probe::TreeNode is already on the path
    CHECK(tree->body() == R"({"id":7,"root":{"name":"top","children":[]}})");
    CHECK(tree->field("root.children").addRow() == 0);  // a truncated collection takes no rows
    CHECK(tree->field("root.children").rows().empty());
}

TEST_CASE("nested: field() walks members and rows and refuses what is not there", "[forms-engine][nested]") {
    Harness harness;
    auto const form = harness.session("T_Grading", kGrading);
    CHECK_THROWS_AS(form->field("rows[0].sieve"), std::out_of_range);
    form->field("rows").addRow();
    CHECK(form->field("rows[0].sieve").spec().name == "sieve");
    CHECK_THROWS_AS(form->field("rows[0]"), std::out_of_range);
    CHECK_THROWS_AS(form->field("rows[x].sieve"), std::out_of_range);
    CHECK_THROWS_AS(form->field("rows[0].nope"), std::out_of_range);
}

// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <morph/forms/engine/form_session.hpp>
#include <optional>
#include <string>
#include <string_view>

#include "forms_engine_support.hpp"

namespace {

using morph::forms::FormSessionOptions;
using morph::test::formsengine::Harness;

constexpr std::string_view kProbe = R"({"type":"object","properties":{
  "slot":{"type":["integer","null"],"x-order":0},
  "mass":{"$ref":"#/$defs/q","x-order":1,"x-decimalPlaces":3,
          "x-unitAlternatives":[{"id":"g","display":"g","decimals":1,"num":1,"den":1000}]},
  "when":{"type":["string","null"],"format":"date-time","x-order":2}},
  "$defs":{"q":{"type":["object","null"],"ExtUnits":{"unitAscii":"kg","unitUnicode":"kg"}}},
  "required":["slot","mass","when"]})";

constexpr std::string_view kTwoInts =
    R"({"properties":{"a":{"type":"integer","x-order":0},"b":{"type":"integer","x-order":1}},"required":["a","b"]})";

}  // namespace

TEST_CASE("FormSession: readiness gates on required fields, precision and date syntax; the body is byte-exact",
          "[forms-engine][session]") {
    Harness harness;
    auto const form = harness.session("Probe", kProbe);
    CHECK_FALSE(form->ready());
    harness.type(*form, "slot", "4");
    harness.type(*form, "when", "2026-07-05T14:30");
    CHECK_FALSE(form->ready());
    harness.type(*form, "mass", "2650.5");
    REQUIRE(form->ready());
    CHECK(form->body() == R"({"slot":4,"mass":{"num":2650500,"den":1000,"dp":3},"when":"2026-07-05T14:30:00Z"})");
    harness.type(*form, "mass", "2650.5001");
    CHECK_FALSE(form->ready());
    CHECK_FALSE(form->body().has_value());
    CHECK(form->field("mass").error().get()->code == "too-precise");
    harness.type(*form, "mass", "2650.5");
    harness.type(*form, "when", "not-a-date");
    CHECK_FALSE(form->ready());
    harness.type(*form, "when", "2026-07-05T14:30:15");
    CHECK(form->ready());
    form->field("mass").switchUnit(1);
    harness.settle();
    CHECK(form->field("mass").text().peek() == "2650500.0");
    CHECK(form->body() == R"({"slot":4,"mass":{"num":26505000,"den":10000,"dp":3},"when":"2026-07-05T14:30:15Z"})");
}

TEST_CASE("FormSession: automatic mode submits once when the body becomes valid, and again only when it changes",
          "[forms-engine][session]") {
    Harness harness;
    auto const form = harness.session("TestAction", kTwoInts);
    harness.type(*form, "a", "3");
    CHECK(harness.server.submits().empty());
    harness.type(*form, "b", "4");
    REQUIRE(harness.server.submits().size() == 1);
    CHECK(harness.server.submits()[0].action == "TestAction");
    CHECK(harness.server.lastBody() == R"({"a":3,"b":4})");
    CHECK(form->lastReply() == R"({"ok":true})");
    harness.type(*form, "b", "4");
    CHECK(harness.server.submits().size() == 1);
    harness.type(*form, "b", "5");
    CHECK(harness.server.submits().size() == 2);
}

TEST_CASE("FormSession: explicit mode submits only on submit(), and only while ready", "[forms-engine][session]") {
    Harness harness;
    auto const form = harness.session(
        "CFR_BookRoom",
        R"({"properties":{"name":{"type":"string","x-order":0}},"required":["name"],"x-submitMode":"explicit"})");
    CHECK(form->model().submitMode() == morph::forms::SubmitMode::Explicit);
    form->submit();
    harness.settle();
    CHECK(harness.server.submits().empty());
    harness.type(*form, "name", "Alice");
    CHECK(form->ready());
    CHECK(harness.server.submits().empty());
    form->submit();
    harness.settle();
    REQUIRE(harness.server.submits().size() == 1);
    CHECK(harness.server.lastBody() == *form->body());
}

TEST_CASE("FormSession: a ready form with nothing to type submits as constructed", "[forms-engine][session]") {
    Harness harness;
    auto const form = harness.session("Test_UnknownRuleKind",
                                      R"({"properties":{"email":{"type":["string","null"],"x-order":0}},"required":[],
          "x-rules":[{"kind":"quantumEntangled","fields":["email"]}]})");
    harness.settle();
    CHECK(form->ready());
    CHECK(harness.server.submits().size() == 1);
}

TEST_CASE("FormSession: prefilling a complete payload never submits; the next user edit does",
          "[forms-engine][session]") {
    Harness harness;
    auto const form = harness.session("TestAction", kTwoInts);
    form->prefill(R"({"a":1,"b":2})");
    harness.settle();
    CHECK(form->ready());
    CHECK(harness.server.submits().empty());
    form->assign("b", "7");
    harness.settle();
    CHECK(form->body() == R"({"a":1,"b":7})");
    CHECK(harness.server.submits().empty());
    harness.type(*form, "a", "9");
    REQUIRE(harness.server.submits().size() == 1);
    CHECK(harness.server.lastBody() == R"({"a":9,"b":7})");
}

TEST_CASE("FormSession: reset clears every draft and does not submit a form that stays ready",
          "[forms-engine][session]") {
    Harness harness;
    auto const form = harness.session(
        "ResetResubmit", R"({"properties":{"note":{"type":["string","null"],"x-order":0}},"required":[]})");
    harness.type(*form, "note", "hello");
    auto const before = harness.server.submits().size();
    (*form).reset();
    harness.settle();
    CHECK(form->ready());
    CHECK(form->field("note").text().peek().empty());
    CHECK(harness.server.submits().size() == before);
}

TEST_CASE("FormSession: a stored payload round-trips, absent members start blank, the unit resets",
          "[forms-engine][session]") {
    Harness harness;
    constexpr std::string_view kSample =
        R"({"$defs":{"double":{"type":"number"},"int64_t":{"type":"integer"}},"properties":{
      "id":{"$ref":"#/$defs/int64_t","x-order":0},
      "density":{"type":["object","null"],"ExtUnits":{"unitAscii":"kg_per_m3","unitUnicode":"kg/m³"},"x-decimalPlaces":2,"x-order":1,
                 "x-unitAlternatives":[{"id":"g_per_cm3","display":"g/cm³","decimals":5,"num":1000,"den":1}]},
      "temperature":{"$ref":"#/$defs/double","x-order":2},
      "takenAt":{"type":"string","format":"date-time","x-order":3},
      "done":{"type":"boolean","x-order":4},
      "role":{"type":"string","oneOf":[{"title":"A","const":"A"},{"title":"B","const":"B"}],"x-order":5},
      "note":{"type":["string","null"],"x-order":6},
      "tags":{"type":"array","items":{"type":"string"},"x-order":7}},
      "required":["id","density","temperature","takenAt","done","role"]})";
    constexpr std::string_view kStored =
        R"({"id":9007199254740993,"density":{"num":245050,"den":100,"dp":2},)"
        R"("temperature":21.5,"takenAt":"2026-07-20T09:00:00Z","done":true,"role":"B",)"
        R"("note":"retest","tags":["a","b"]})";
    auto const form = harness.session("T_Sample", kSample);
    form->field("density").switchUnit(1);
    form->prefill(kStored);
    harness.settle();
    CHECK(form->body() == kStored);
    CHECK(harness.server.submits().empty());
    CHECK(form->field("id").text().peek() == "9007199254740993");
    CHECK(form->field("density").text().peek() == "2450.50");
    CHECK(form->field("density").unit().peek() == 0);
    CHECK(form->field("takenAt").text().peek() == "2026-07-20T09:00:00");
    CHECK(form->field("role").text().peek() == R"("B")");
    CHECK(form->field("tags").text().peek() == "a, b");
    harness.type(*form, "temperature", "22.00");
    CHECK(form->body()->contains(R"("temperature":22.00)"));

    form->prefill(R"({"id":5})");
    harness.settle();
    CHECK(form->field("temperature").text().peek().empty());
    CHECK_FALSE(form->ready());
    std::string const before = form->field("id").text().peek();
    form->prefill("not json");
    form->prefill("[1,2]");
    harness.settle();
    CHECK(form->field("id").text().peek() == before);

    Harness german;
    auto const germanForm = german.session(
        "T_Sample", kSample, FormSessionOptions{.locale = {.decimalSeparator = ",", .groupSeparator = "."}});
    germanForm->prefill(kStored);
    german.settle();
    CHECK(germanForm->field("density").text().peek() == "2450,50");
    CHECK(germanForm->field("temperature").text().peek() == "21,5");
    CHECK(germanForm->body() == kStored);

    Harness zoned;
    auto const shifted = zoned.session("T_Sample", kSample, FormSessionOptions{.displayOffsetMinutes = 120});
    shifted->prefill(kStored);
    zoned.settle();
    CHECK(shifted->field("takenAt").text().peek() == "2026-07-20T11:00:00");
    CHECK(shifted->body()->contains(R"("takenAt":"2026-07-20T09:00:00Z")"));
}

TEST_CASE("FormSession: x-blankAs empty submits \"\" once the field was filled, and only then",
          "[forms-engine][session]") {
    constexpr std::string_view kEdit = R"({"type":"object","properties":{
      "id":{"type":"integer","x-order":0},
      "remark":{"type":["string","null"],"x-order":1,"x-blankAs":"empty"},
      "operatorName":{"anyOf":[{"type":"string"},{"type":"null"}],"x-order":2,"x-blankAs":"empty"},
      "plain":{"type":["string","null"],"x-order":3},
      "weight":{"type":["number","null"],"x-order":4,"x-blankAs":"empty"},
      "title":{"type":"string","x-order":5,"x-blankAs":"empty"}},"required":["id","title"]})";
    constexpr std::string_view kStored =
        R"({"id":7,"remark":"abc","operatorName":"Ann","plain":"keep","weight":1.5,"title":"T"})";
    Harness harness;
    auto const form = harness.session("T_EditSample", kEdit);
    CHECK(form->field("remark").spec().blankAs == morph::forms::BlankAs::Empty);
    CHECK(form->field("weight").spec().blankAs == morph::forms::BlankAs::Omit);  // not a string field
    form->prefill(kStored);
    harness.settle();
    CHECK(harness.server.submits().empty());
    harness.type(*form, "remark", "");
    CHECK(harness.server.lastBody() ==
          R"({"id":7,"remark":"","operatorName":"Ann","plain":"keep","weight":1.5,"title":"T"})");
    form->prefill(kStored);
    harness.settle();
    harness.type(*form, "plain", "");
    CHECK(harness.server.lastBody() == R"({"id":7,"remark":"abc","operatorName":"Ann","weight":1.5,"title":"T"})");
    form->prefill(kStored);
    harness.settle();
    harness.type(*form, "weight", "");
    CHECK(harness.server.lastBody() == R"({"id":7,"remark":"abc","operatorName":"Ann","plain":"keep","title":"T"})");
    auto const before = harness.server.submits().size();
    harness.type(*form, "title", "");
    CHECK_FALSE(form->ready());
    CHECK(harness.server.submits().size() == before);

    (*form).reset();
    harness.settle();
    harness.type(*form, "id", "8");
    harness.type(*form, "title", "U");
    CHECK(harness.server.lastBody() == R"({"id":8,"title":"U"})");
    harness.type(*form, "operatorName", "Bo");
    harness.type(*form, "operatorName", "");
    CHECK(harness.server.lastBody() == R"({"id":8,"operatorName":"","title":"U"})");

    form->prefill(R"({"id":7,"remark":"","title":"T"})");
    harness.settle();
    CHECK(form->body() == R"({"id":7,"remark":"","title":"T"})");
    form->prefill(R"({"id":7,"remark":null,"title":"T"})");
    harness.settle();
    CHECK(form->body() == R"({"id":7,"title":"T"})");
}

TEST_CASE("FormSession: booleans are bare literals; a required one answers false, an optional one is omitted",
          "[forms-engine][session]") {
    Harness harness;
    auto const required = harness.session(
        "T_Bool",
        R"({"properties":{"note":{"type":"string","x-order":0},"flag":{"type":"boolean","x-order":1}},"required":["note","flag"]})");
    harness.type(*required, "note", "n");
    CHECK(required->body() == R"({"note":"n","flag":false})");
    harness.type(*required, "flag", "true");
    CHECK(required->body() == R"({"note":"n","flag":true})");
    auto const optional = harness.session(
        "T_NullableBool", R"({"properties":{"flag":{"type":["boolean","null"],"x-order":0}},"required":[]})");
    harness.settle();
    CHECK(optional->body() == "{}");
    harness.type(*optional, "flag", "true");
    CHECK(optional->body() == R"({"flag":true})");
}

TEST_CASE("FormSession: an integer through anyOf keeps every digit and refuses text", "[forms-engine][session]") {
    Harness harness;
    auto const form =
        harness.session("T_AnyOf", R"({"$defs":{"int64_t":{"type":"integer"},"TagId":{"type":"integer"}},"properties":{
      "optI64":{"anyOf":[{"$ref":"#/$defs/int64_t"},{"type":"null"}],"x-order":0},
      "refI64":{"$ref":"#/$defs/TagId","x-order":1}},"required":[]})");
    harness.type(*form, "optI64", "9007199254740993");
    CHECK(form->body() == R"({"optI64":9007199254740993})");
    harness.type(*form, "optI64", "9223372036854775807");
    harness.type(*form, "refI64", "18446744073709551615");
    CHECK(form->body() == R"({"optI64":9223372036854775807,"refI64":18446744073709551615})");
    harness.type(*form, "optI64", "banana");
    CHECK_FALSE(form->ready());
}

TEST_CASE("FormSession: an id beyond 2^53 is exact through encode, body, a rule and prefill",
          "[forms-engine][session]") {
    Harness harness;
    auto const form = harness.session("RcEqualsBigInt", R"({"$defs":{"int64_t":{"type":"integer"}},"properties":{
      "id":{"anyOf":[{"$ref":"#/$defs/int64_t"},{"type":"null"}],"x-order":0},
      "note":{"type":["string","null"],"x-order":1}},"required":[],
      "x-rules":[{"kind":"requiredWhen","fields":["note"],"when":{"kind":"equals","fields":["id"],"value":9007199254740993,"valueText":"9007199254740993"}}]})");
    harness.type(*form, "id", "9007199254740992");
    CHECK(form->ready());
    harness.type(*form, "id", "9007199254740993");
    CHECK_FALSE(form->ready());
    CHECK(form->field("note").required().get());
    form->prefill(R"({"id":9007199254740993,"note":"why"})");
    harness.settle();
    CHECK(form->body() == R"({"id":9007199254740993,"note":"why"})");
}

TEST_CASE("FormSession: x-rules gate and present live", "[forms-engine][session]") {
    Harness harness;
    auto const form = harness.session("CFR_BookRoom", R"({"properties":{
      "name":{"type":"string","x-order":0},"checkInN":{"type":"integer","x-order":1},"checkOutN":{"type":"integer","x-order":2},
      "email":{"type":"string","x-order":3},"phone":{"type":"string","x-order":4},
      "promo":{"type":"integer","x-order":5},"discount":{"type":"integer","x-order":6}},"required":["name"],
      "x-rules":[{"kind":"greater","fields":["checkOutN","checkInN"]},{"kind":"exactlyOneOf","fields":["email","phone"]},
        {"kind":"requiredWhen","fields":["discount"],"when":{"kind":"engaged","fields":["promo"]}},
        {"kind":"visibleWhen","fields":["discount"],"when":{"kind":"engaged","fields":["promo"]}}]})");
    CHECK_FALSE(form->field("discount").visible().get());
    harness.type(*form, "name", "Alice");
    CHECK_FALSE(form->ready());
    harness.type(*form, "email", "a@b.com");
    CHECK(form->ready());
    CHECK(harness.server.submits().size() == 1);
    harness.type(*form, "promo", "5");
    CHECK(form->field("discount").visible().get());
    CHECK(form->field("discount").required().get());
    CHECK_FALSE(form->ready());
    CHECK(harness.server.submits().size() == 1);
    harness.type(*form, "discount", "2");
    CHECK(form->ready());
    CHECK(harness.server.submits().size() == 2);
    harness.type(*form, "checkInN", "10");
    harness.type(*form, "checkOutN", "3");
    CHECK_FALSE(form->ready());
    harness.type(*form, "checkOutN", "20");
    CHECK(form->ready());
    harness.type(*form, "phone", "555");
    CHECK_FALSE(form->ready());
}

TEST_CASE("FormSession: compound when-clauses require, release and negate", "[forms-engine][session]") {
    auto const schema = [](std::string_view when) {
        return std::string{
                   R"({"properties":{"name":{"type":"string","x-order":0},"promo":{"type":"integer","x-order":1},
          "loyaltyCode":{"type":"integer","x-order":2},"discount":{"type":"integer","x-order":3}},"required":["name"],
          "x-rules":[{"kind":"requiredWhen","fields":["discount"],"when":)"} +
               std::string{when} + "}]}";
    };
    Harness harness;
    auto const both = harness.session(
        "A",
        schema(
            R"({"kind":"and","conditions":[{"kind":"engaged","fields":["promo"]},{"kind":"engaged","fields":["loyaltyCode"]}]})"));
    harness.type(*both, "name", "Alice");
    harness.type(*both, "promo", "5");
    CHECK(both->ready());
    harness.type(*both, "loyaltyCode", "9");
    CHECK_FALSE(both->ready());
    harness.type(*both, "discount", "2");
    CHECK(both->ready());
    auto const either = harness.session(
        "O",
        schema(
            R"({"kind":"or","conditions":[{"kind":"engaged","fields":["promo"]},{"kind":"engaged","fields":["loyaltyCode"]}]})"));
    harness.type(*either, "name", "Alice");
    CHECK(either->ready());
    harness.type(*either, "promo", "5");
    CHECK_FALSE(either->ready());
    auto const negated =
        harness.session("N", schema(R"({"kind":"not","condition":{"kind":"engaged","fields":["promo"]}})"));
    harness.type(*negated, "name", "Alice");
    CHECK_FALSE(negated->ready());
    harness.type(*negated, "promo", "5");
    CHECK(negated->ready());
}

TEST_CASE("FormSession: an enum starts unselected, encodes the chosen literal and refuses anything else",
          "[forms-engine][session]") {
    Harness harness;
    harness.server.replyWith([](auto const&) { return std::nullopt; });  // record only
    auto const form = harness.session("SetMemberRole", R"({"properties":{
      "principal":{"type":"string","x-order":1},"projectId":{"type":["integer","null"],"x-order":0},
      "role":{"type":"string","oneOf":[{"title":"Viewer","const":"Viewer"},{"title":"Member","const":"Member"},{"title":"Manager","const":"Manager"}],"x-order":2}},
      "required":["projectId","principal","role"]})");
    harness.type(*form, "projectId", "1");
    harness.type(*form, "principal", "bob");
    CHECK_FALSE(form->ready());
    CHECK(form->field("role").options().size() == 3);
    harness.type(*form, "role", R"("Emperor")");
    CHECK_FALSE(form->ready());
    CHECK(harness.server.submits().empty());
    harness.type(*form, "role", "Manager");
    CHECK_FALSE(form->ready());
    harness.type(*form, "role", R"("Manager")");
    CHECK(form->body() == R"({"projectId":1,"principal":"bob","role":"Manager"})");
    (*form).reset();
    harness.settle();
    CHECK(form->field("role").text().peek().empty());
    CHECK(form->field("principal").text().peek().empty());
}

TEST_CASE("FormSession: an array field encodes a JSON array; blank optional is omitted, a required one gates",
          "[forms-engine][session]") {
    Harness harness;
    constexpr std::string_view kProps =
        R"("name":{"type":"string","x-order":0},"tags":{"type":"array","items":{"type":"string"},"x-order":1})";
    auto const optional = harness.session(
        "CFR_TagRoom", std::string{R"({"properties":{)"} + std::string{kProps} + R"(},"required":["name"]})");
    harness.type(*optional, "name", "Alice");
    CHECK(optional->body() == R"({"name":"Alice"})");
    harness.type(*optional, "tags", "solo");
    CHECK(optional->body() == R"({"name":"Alice","tags":["solo"]})");
    auto const required = harness.session(
        "CFR_TagRoom", std::string{R"({"properties":{)"} + std::string{kProps} + R"(},"required":["name","tags"]})");
    harness.type(*required, "name", "Alice");
    CHECK_FALSE(required->ready());
    harness.type(*required, "tags", " , , ");
    CHECK(required->body() == R"({"name":"Alice","tags":[]})");
}

TEST_CASE("FormSession: labels resolve through the translations; numbers read in the session locale",
          "[forms-engine][session]") {
    morph::render::TranslationProvider const catalog = [](std::string_view key,
                                                          std::string_view locale) -> std::optional<std::string> {
        if (locale != "de") {
            return std::nullopt;
        }
        if (key == "Probe.slot.label") {
            return "Steckplatz";
        }
        if (key == "custom.stem.label") {
            return "Übersteuert";
        }
        return std::nullopt;
    };
    constexpr std::string_view kSchema = R"({"properties":{"slot":{"type":["integer","null"],"x-order":0},
      "mass":{"$ref":"#/$defs/q","x-order":1,"x-decimalPlaces":3,"ExtUnits":{"unitAscii":"kg","unitUnicode":"kg"}},
      "other":{"type":"integer","x-order":2,"x-i18nKey":"custom.stem"}},"$defs":{"q":{"type":["object","null"]}},"required":["mass"]})";
    morph::render::NumericLocale const german{.decimalSeparator = ",", .groupSeparator = "."};
    Harness harness;
    auto const form = harness.session("Probe", kSchema,
                                      FormSessionOptions{.locale = german, .bcp47 = "de", .translations = catalog});
    CHECK(form->field("slot").label() == "Steckplatz");
    CHECK(form->field("mass").label() == "mass");
    CHECK(form->field("other").label() == "Übersteuert");
    auto const plain = harness.session("Probe", kSchema);
    CHECK(plain->field("slot").label() == "slot");

    harness.type(*form, "mass", "1.050,25");
    CHECK(form->body() == R"({"mass":{"num":1050250,"den":1000,"dp":3}})");
    for (std::string_view const foreign : {"1.5", "1.50", "1234.050,25"}) {
        harness.type(*form, "mass", foreign);
        CHECK_FALSE(form->ready());
    }
    harness.type(*form, "mass", "+1.050,25");
    CHECK(form->body() == R"({"mass":{"num":1050250,"den":1000,"dp":3}})");
    constexpr std::string_view kMinus = "\xE2\x88\x92";  // U+2212
    morph::render::NumericLocale const basque{.decimalSeparator = ",", .groupSeparator = ".", .negativeSign = kMinus};
    auto const signForm = harness.session("Probe", kSchema, FormSessionOptions{.locale = basque});
    harness.type(*signForm, "mass", std::string{kMinus} + "5");
    CHECK(signForm->body() == R"({"mass":{"num":-5000,"den":1000,"dp":3}})");
    harness.type(*signForm, "mass", "-5");
    CHECK(signForm->body() == R"({"mass":{"num":-5000,"den":1000,"dp":3}})");
}

TEST_CASE("FormSession: a failed submission is the last error; a later success clears it", "[forms-engine][session]") {
    Harness harness;
    harness.server.replyWith([](auto const&) { return std::nullopt; });
    auto const form = harness.session("TestAction", kTwoInts);
    harness.type(*form, "a", "1");
    harness.type(*form, "b", "2");
    CHECK(form->pending());
    harness.server.rejectSubmit(0, "denied");
    harness.settle();
    CHECK_FALSE(form->pending());
    CHECK(morph::reactive::errorMessage(form->lastError()) == "denied");
    harness.type(*form, "b", "3");
    harness.server.resolveSubmit(1, R"({"sum":4})");
    harness.settle();
    CHECK(form->lastError() == nullptr);
    CHECK(form->lastReply() == R"({"sum":4})");
}

TEST_CASE("FormSession: field() resolves paths and refuses unknown ones", "[forms-engine][session]") {
    Harness harness;
    auto const form = harness.session("TestAction", kTwoInts);
    CHECK(form->field("a").path() == "a");
    CHECK_THROWS_AS(form->field("nope"), std::out_of_range);
    CHECK_THROWS_AS(form->field("a.b"), std::out_of_range);
}

// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <morph/forms/engine/form_session.hpp>
#include <string>
#include <string_view>

#include "forms_engine_support.hpp"

namespace {

using morph::test::formsengine::Harness;
using morph::test::formsengine::modelOf;

constexpr std::string_view kShipTo = R"({"properties":{
  "country":{"type":["integer","null"],"x-order":0,"x-optionsAction":"ListCountries","x-optionValue":"id","x-optionLabel":"name"},
  "city":{"type":["integer","null"],"x-order":1,"x-optionsAction":"ListCities","x-optionValue":"id","x-optionLabel":"name","x-optionsDependsOn":["country"]}},
  "required":["country","city"]})";

/// Countries 1 and 2; cities 10 in country 1 and nothing else.
[[nodiscard]] std::optional<std::string> geography(morph::test::formsengine::Call const& call) {
    if (call.action == "ListCountries") {
        return R"([{"id":1,"name":"One"},{"id":2,"name":"Two"}])";
    }
    if (call.body == R"({"country":1})") {
        return R"([{"id":10,"name":"Ten"}])";
    }
    return "[]";
}

[[nodiscard]] std::vector<std::string> labels(morph::forms::FieldState const& field) {
    std::vector<std::string> out;
    for (auto const& option : field.options()) {
        out.push_back(option.label);
    }
    return out;
}

}  // namespace

TEST_CASE("choice: only an independent Choice fetches on construction, with an empty body", "[forms-engine][choice]") {
    Harness harness;
    auto const form = harness.session("ShipTo", kShipTo);
    REQUIRE(harness.server.fetches().size() == 1);
    CHECK(harness.server.fetches()[0].action == "ListCountries");
    CHECK(harness.server.fetches()[0].body == "{}");
}

TEST_CASE("choice: a dependent Choice refetches with its parents' literals when a parent changes",
          "[forms-engine][choice]") {
    Harness harness;
    harness.server.optionsWith(geography);
    auto const form = harness.session("ShipTo", kShipTo);
    harness.type(*form, "country", "1");
    REQUIRE(harness.server.fetches().size() == 2);
    CHECK(harness.server.fetches()[1].action == "ListCities");
    CHECK(harness.server.fetches()[1].body == R"({"country":1})");
    harness.type(*form, "city", "10");
    harness.type(*form, "country", "");
    CHECK(harness.server.fetches().size() == 2);  // a blank parent is no request
    // ...and an idle query has fetched nothing to judge the selection against, so it is kept.
    CHECK(form->field("city").text().peek() == "10");
}

TEST_CASE("choice: a selection the new options no longer back is cleared, without submitting",
          "[forms-engine][choice]") {
    Harness harness;
    harness.server.optionsWith(geography);
    auto const form = harness.session("ShipTo", kShipTo);
    harness.type(*form, "country", "1");
    harness.type(*form, "city", "10");
    CHECK(form->field("city").text().peek() == "10");
    auto const submitted = harness.server.submits().size();
    harness.type(*form, "country", "2");
    CHECK(form->field("city").text().peek().empty());
    CHECK(harness.server.submits().size() == submitted);
}

TEST_CASE("choice: a reply for a superseded parent value is dropped, and a prefilled selection survives its refetch",
          "[forms-engine][choice]") {
    Harness harness;
    harness.server.optionsWith([](auto const&) { return std::nullopt; });  // settle by hand
    auto const form = harness.session("ShipTo", kShipTo);
    harness.server.resolveFetch(0, R"([{"id":1,"name":"A"},{"id":2,"name":"B"}])");
    harness.settle();
    CHECK(labels(form->field("country")) == std::vector<std::string>{"A", "B"});
    harness.type(*form, "country", "1");
    harness.type(*form, "country", "2");
    REQUIRE(harness.server.fetches().size() == 3);
    CHECK(form->field("city").optionsPending());
    harness.server.resolveFetch(1, R"([{"id":10,"name":"Old"}])");
    harness.settle();
    CHECK(form->field("city").options().empty());  // the reply for country 1 is stale
    harness.server.resolveFetch(2, R"([{"id":20,"name":"New"}])");
    harness.settle();
    CHECK(labels(form->field("city")) == std::vector<std::string>{"New"});

    form->prefill(R"({"country":1,"city":10})");
    harness.settle();
    REQUIRE(harness.server.fetches().size() == 4);
    CHECK(form->field("city").text().peek() == "10");  // kept while its options are in flight
    harness.server.resolveFetch(3, R"([{"id":10,"name":"Old"}])");
    harness.settle();
    CHECK(form->field("city").text().peek() == "10");
    CHECK(form->body() == R"({"country":1,"city":10})");
}

TEST_CASE("choice: a failed fetch keeps the selection; an answered one without it clears it",
          "[forms-engine][choice]") {
    Harness harness;
    harness.server.optionsWith([](auto const&) { return std::nullopt; });
    auto const form = harness.session(
        "Probe",
        R"({"properties":{"slot":{"type":"integer","x-optionsAction":"ListSlots","x-order":0}},"required":["slot"]})");
    form->prefill(R"({"slot":4})");
    harness.settle();
    harness.server.rejectFetch(0, "options unavailable");
    harness.settle();
    CHECK(form->field("slot").text().peek() == "4");
    CHECK(form->body() == R"({"slot":4})");

    Harness answered;
    answered.server.optionsWith([](auto const&) { return std::optional<std::string>{R"([{"id":5,"name":"Five"}])"}; });
    auto const other = answered.session(
        "Probe",
        R"({"properties":{"slot":{"type":"integer","x-optionsAction":"ListSlots","x-order":0}},"required":["slot"]})");
    answered.settle();
    other->prefill(R"({"slot":4})");
    answered.settle();
    CHECK(other->field("slot").text().peek().empty());
}

TEST_CASE("choice: option values and labels keep ids beyond 2^53 distinct", "[forms-engine][choice]") {
    Harness harness;
    constexpr std::string_view kRows =
        R"({"rows":[{"id":9007199254740993,"name":"Alpha"},{"id":9007199254740992,"name":"Beta"}]})";
    harness.server.optionsWith([&](auto const&) { return std::optional<std::string>{kRows}; });
    auto const byName = harness.session(
        "Probe",
        R"({"properties":{"slot":{"type":["integer","null"],"x-order":0,"x-optionsAction":"ListRows","x-optionValue":"id","x-optionLabel":"name"}},"required":["slot"]})");
    auto const byId = harness.session(
        "Probe",
        R"({"properties":{"slot":{"type":["integer","null"],"x-order":0,"x-optionsAction":"ListRows","x-optionValue":"id","x-optionLabel":"id"}},"required":["slot"]})");
    harness.settle();
    auto const& options = byName->field("slot").options();
    REQUIRE(options.size() == 2);
    CHECK(options[0].valueJson == "9007199254740993");
    CHECK(options[1].valueJson == "9007199254740992");
    CHECK(labels(byId->field("slot")) == std::vector<std::string>{"9007199254740993", "9007199254740992"});
    harness.type(*byName, "slot", options[0].valueJson);
    CHECK(byName->body() == R"({"slot":9007199254740993})");
}

TEST_CASE("choice: a prefilled selection is shown once its options arrive", "[forms-engine][choice]") {
    Harness harness;
    harness.server.optionsWith([](auto const&) {
        return std::optional<std::string>{R"([{"id":9007199254740993,"name":"Big"},{"id":2,"name":"Two"}])"};
    });
    auto const form = harness.session(
        "T_Choice",
        R"({"properties":{"sample":{"type":"integer","x-optionsAction":"ListSamples","x-order":0}},"required":["sample"]})");
    form->prefill(R"({"sample":9007199254740993})");
    harness.settle();
    CHECK(form->body() == R"({"sample":9007199254740993})");
    auto const& options = form->field("sample").options();
    REQUIRE(options.size() == 2);
    CHECK(options[0].valueJson == form->field("sample").text().peek());
    CHECK(options[0].label == "Big");
}

TEST_CASE("choice: a form with no Choice needs no fetcher; one with a Choice gets its options",
          "[forms-engine][choice]") {
    Harness harness;
    morph::forms::FormSession const plain{
        harness.rt,
        modelOf(
            "CreateNote",
            R"({"properties":{"title":{"type":"string","x-order":0},"count":{"type":"integer","x-order":1}},"required":["title"]})"),
        harness.server.submitter(), morph::forms::ChoiceFetcher{}};
    CHECK(plain.fields().size() == 2);
    CHECK(harness.server.fetches().empty());

    harness.server.optionsWith(
        [](auto const&) { return std::optional<std::string>{R"([{"id": 4, "name": "Bravo"}])"}; });
    auto const choice = harness.session(
        "MoveCard",
        R"({"properties":{"columnId":{"type":"integer","x-order":0,"x-optionsAction":"ListColumns","x-optionValue":"id","x-optionLabel":"name"}},"required":["columnId"]})");
    harness.settle();
    CHECK(labels(choice->field("columnId")) == std::vector<std::string>{"Bravo"});
}

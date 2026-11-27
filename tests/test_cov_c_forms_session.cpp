// SPDX-License-Identifier: Apache-2.0
//
// How a form session reads the field errors of a save's reply, and what a reply that names them does to what was
// sent.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <glaze/glaze.hpp>
#include <morph/forms/derive.hpp>
#include <morph/forms/engine/form_session.hpp>
#include <vector>
#include <optional>
#include <string>
#include <string_view>

#include "forms_engine_support.hpp"

namespace {

using morph::test::formsengine::Harness;

constexpr std::string_view kThree = R"({"properties":{"a":{"type":"integer","x-order":0},
  "b":{"type":"integer","x-order":1},"c":{"type":"integer","x-order":2}},"required":["a","b","c"],
  "x-submitMode":"explicit"})";

}  // namespace

TEST_CASE("a reply's field errors show at the fields they name, in either of two shapes", "[covc][forms][session]") {
    Harness harness;
    harness.server.replyWith([](auto const&) {
        return std::optional<std::string>{
            R"({"errors":{"a":"too big","b":["first","second"],"c":[],"zz":"no such field","a2":5}})"};
    });
    auto const form = harness.session("TestAction", kThree);
    harness.type(*form, "a", "1");
    harness.type(*form, "b", "2");
    harness.type(*form, "c", "3");
    form->submit();
    harness.settle();
    CHECK(form->refusal("a") == std::optional<std::string>{"too big"});
    CHECK(form->refusal("b") == std::optional<std::string>{"first"});
    CHECK_FALSE(form->refusal("c"));
    CHECK_FALSE(form->refusal("zz"));
}

TEST_CASE("a reply with no errors member, or one that is not an object, refuses nothing", "[covc][forms][session]") {
    for (std::string const reply : {R"({"ok":true})", R"({"errors":[1]})", R"(not json)", R"({"errors":{"a":""}})"}) {
        CAPTURE(reply);
        Harness harness;
        harness.server.replyWith([reply](auto const&) { return std::optional<std::string>{reply}; });
        auto const form = harness.session("TestAction", kThree);
        harness.type(*form, "a", "1");
        harness.type(*form, "b", "2");
        harness.type(*form, "c", "3");
        form->submit();
        harness.settle();
        CHECK_FALSE(form->refusal("a"));
    }
}

TEST_CASE("a save refused for a field stays an edit while a clean save is taken as loaded", "[covc][forms][session]") {
    Harness harness;
    bool refuse = true;
    harness.server.replyWith([&refuse](auto const&) {
        return std::optional<std::string>{refuse ? R"({"errors":{"a":"no"}})" : R"({"ok":true})"};
    });
    auto const form = harness.session("TestAction", kThree);
    harness.type(*form, "a", "1");
    harness.type(*form, "b", "2");
    harness.type(*form, "c", "3");
    form->submit();
    harness.settle();
    form->refill(R"({"a":9,"b":9,"c":9})");
    harness.settle();
    CHECK(form->field("a").text().peek() == "1");
    refuse = false;
    form->submit();
    harness.settle();
    form->refill(R"({"a":9,"b":9,"c":9})");
    harness.settle();
    CHECK(form->field("a").text().peek() == "9");
}

namespace {

constexpr std::string_view kDerived = R"({"type":"object","properties":{
  "name":{"type":"string","x-order":0},
  "limit":{"type":"integer","x-derived":true,"x-order":1},
  "sum":{"type":"integer","x-computed":{"inputs":["boxes"]},"x-order":2},
  "crate":{"type":"object","x-order":3,"properties":{
    "width":{"type":"integer","x-order":0},
    "area":{"type":"integer","x-computed":{},"x-order":1}}},
  "boxes":{"type":"array","x-order":4,"items":{"type":"object","properties":{
    "count":{"type":"integer","x-order":0},
    "cap":{"type":"integer","x-derived":true,"x-order":1},
    "total":{"type":"integer","x-computed":{},"x-order":2}}}}},
  "required":["name"]})";

}  // namespace

TEST_CASE("a round of derived values is resolved against the rows it was sent for", "[covc][forms][session]") {
    Harness harness;
    auto const form = harness.session("Derived", kDerived);
    auto const first = form->field("boxes").addRow();
    auto const second = form->field("boxes").addRow();
    harness.settle();
    auto const round = form->captureRound();
    REQUIRE(round.rowKeys.contains("boxes"));
    CHECK(round.rowKeys.at("boxes") == std::vector<std::uint64_t>{first, second});
    form->field("boxes").removeRow(first);
    harness.settle();

    auto const value = [](std::string path, std::string json) {
        return morph::forms::Derived::Value{.path = std::move(path), .value = glz::raw_json{std::move(json)}};
    };
    morph::forms::Derived reply;
    reply.values = {value("limit", "9"),
                    value("boxes[0].cap", "5"),
                    value("boxes[1].cap", "6"),
                    value("boxes[1].count", "7"),
                    value("boxes[7].cap", "1"),
                    value("boxes[x].cap", "1"),
                    value("boxes[1.cap", "1"),
                    value("name[0].cap", "1"),
                    value("boxes[1]", "1"),
                    value("nosuch", "1"),
                    value("crate", "{}"),
                    value("limit", "not json"),
                    value("crate.width.deeper", "1")};
    auto const load = form->loadDerived(round, reply);
    CHECK(load.loaded == 2);
    CHECK(load.refused == std::vector<std::string>{"boxes[1].count", "boxes[7].cap", "boxes[x].cap", "boxes[1.cap",
                                                   "name[0].cap", "boxes[1]", "nosuch", "crate", "limit",
                                                   "crate.width.deeper"});
    CHECK(form->field("limit").text().peek() == "9");

    CHECK(form->loadDerived(round, reply).loaded == 2);
    auto const stale = morph::forms::DeriveRound{.sequence = 0, .body = {}, .rowKeys = {}};
    CHECK(form->loadDerived(stale, reply).loaded == 0);
}

TEST_CASE("an evaluation's computed values land on the fields and rows it was sent for", "[covc][forms][session]") {
    Harness harness;
    auto const form = harness.session("Derived", kDerived);
    form->field("boxes").addRow();
    form->field("boxes").addRow();
    harness.settle();
    auto const round = form->captureRound();
    auto const body = R"({"sum":3,"crate":{"area":16},"boxes":[{"total":1},{"total":2},{"total":99}]})";
    auto const load = form->loadDerived(round, std::string_view{body});
    CHECK(load.loaded == 4);
    CHECK(form->field("sum").text().peek() == "3");

    auto const later = form->captureRound();
    CHECK(form->loadDerived(later, std::string_view{R"([1])"}).loaded == 0);
    CHECK(form->loadDerived(later, std::string_view{"not json"}).loaded == 0);
    CHECK(form->loadDerived(later, std::string_view{R"({"crate":5,"boxes":{"x":1}})"}).loaded == 0);
    CHECK(form->loadDerived(later, std::string_view{R"({"boxes":[5,{"total":8}]})"}).loaded == 1);
    CHECK(form->loadDerived(round, std::string_view{body}).loaded == 0);
}

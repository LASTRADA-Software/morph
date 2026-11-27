// SPDX-License-Identifier: Apache-2.0
//
// How a form session reads the field errors of a save's reply, and what a reply that names them does to what was
// sent.

#include <catch2/catch_test_macros.hpp>
#include <morph/forms/engine/form_session.hpp>
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

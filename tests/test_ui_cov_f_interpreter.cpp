// SPDX-License-Identifier: Apache-2.0
//
// A screen's own state across a replacement, the answer to a request to close it, and what a head action refuses.

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <morph/ui/interpreter.hpp>
#include <morph/ui/value.hpp>
#include <string>
#include <string_view>

#include "ui_interpreter_support.hpp"

namespace ui = morph::ui;
using ui::testing::Harness;

namespace {

constexpr std::string_view kCounter = R"({
  "vocab": "ui/1", "id": "counter",
  "state": { "n": { "type": "int", "init": 0 }, "label": { "init": "a" }, "unread": { "init": 7 } },
  "actions": [ { "id": "bump", "label": "Bump", "onClick": [ { "set": "n", "value": 5 }, { "set": "label", "value": "b" } ] } ],
  "view": { "kind": "column", "children": [
    { "kind": "text", "testId": "n", "text": { "ref": "n" } },
    { "kind": "text", "testId": "label", "text": { "ref": "label" } } ] } })";

// The same names, the state `n` now text.
constexpr std::string_view kRetyped = R"({
  "vocab": "ui/1", "id": "counter",
  "state": { "n": { "type": "string", "init": "zero" }, "label": { "init": "a" } },
  "view": { "kind": "column", "children": [
    { "kind": "text", "testId": "n", "text": { "ref": "n" } },
    { "kind": "text", "testId": "label", "text": { "ref": "label" } } ] } })";

constexpr std::string_view kClosable = R"({
  "vocab": "ui/1", "id": "closable", "state": { "veto": { "init": false } },
  "onClose": [ { "if": [ { "ref": "veto" }, [ { "cancelClose": true } ] ] } ],
  "actions": [ { "id": "veto", "label": "Veto", "onClick": [ { "set": "veto", "value": true } ] } ],
  "view": { "kind": "text", "testId": "v", "text": { "ref": "veto" } } })";

constexpr std::string_view kPlain = R"({
  "vocab": "ui/1", "id": "plain", "view": { "kind": "text", "testId": "t", "text": "x" } })";

}  // namespace

TEST_CASE("a screen that replaces another keeps the states of the same name and type, and starts the rest anew",
          "[covf][ui][interpreter]") {
    Harness h;
    auto const first = h.mount(kCounter);
    CHECK(h.text("n") == "0");
    CHECK(first->runAction("bump"));
    h.settle();
    CHECK(h.text("n") == "5");
    CHECK(h.text("label") == "b");

    auto const same = h.interpreter.mount(ui::testing::loadOrFail(kCounter, h.catalog), {}, h.backend,
                                          ui::MountOptions{.keepStateOf = first.get()});
    REQUIRE(same.has_value());
    h.settle();
    CHECK(ui::testing::readJson(**same, R"({"ref":"n"})") == "5");
    CHECK(ui::testing::readJson(**same, R"({"ref":"label"})") == R"("b")");

    auto const retyped = h.interpreter.mount(ui::testing::loadOrFail(kRetyped, h.catalog), {}, h.backend,
                                             ui::MountOptions{.keepStateOf = first.get()});
    REQUIRE(retyped.has_value());
    h.settle();
    CHECK(ui::testing::readJson(**retyped, R"({"ref":"n"})") == R"("zero")");
    CHECK(ui::testing::readJson(**retyped, R"({"ref":"label"})") == R"("b")");
}

TEST_CASE("a screen asked to close closes unless its onClose cancels it", "[covf][ui][interpreter]") {
    Harness h;
    auto const closable = h.mount(kClosable);
    CHECK(closable->requestClose());
    CHECK(closable->runAction("veto"));
    h.settle();
    CHECK_FALSE(closable->requestClose());

    Harness plain;
    auto const screen = plain.mount(kPlain);
    CHECK(screen->requestClose());
    CHECK(screen->beginClose() == nullptr);
}

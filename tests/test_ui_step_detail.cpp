// SPDX-License-Identifier: Apache-2.0

// A step's second line (`Step::detail`): the mount, a bound `items` and a document's `detail` field reach the backend,
// as the recording backend shows them.

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/signal.hpp>
#include <morph/testing/step_executor.hpp>
#include <morph/ui/mount.hpp>
#include <morph/ui/testing/recording_backend.hpp>
#include <morph/ui/view.hpp>
#include <optional>
#include <string>
#include <vector>

#include "test_support.hpp"
#include "ui_interpreter_support.hpp"
#include "ui_test_support.hpp"

namespace ui = morph::ui;

namespace {

using morph::reactive::Runtime;
using morph::reactive::Signal;
using ui::testing::RecordingBackend;
using Owner = morph::testing::StepExecutor;
using Lines = std::vector<std::string>;

}  // namespace

// Mutation: leave `detail` out of the recording (the first check fails), or out of Step's equality (a changed detail
// is then not sent again).
TEST_CASE("step detail: a step's detail reaches the backend, follows its binding and is empty by default", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<std::string> who{runtime, "A. Weber, 09:14"};
    ui::Mounted const view{runtime, backend, ui::steps({.items = [&] {
                               return std::vector<ui::Step>{
                                   {.label = "Received", .state = ui::StepState::Done, .detail = who.get()},
                                   {.label = "Verified", .state = ui::StepState::Active}};
                           }})};
    CHECK(backend.prop(1, "steps") == "[Received:Done;detail=A. Weber\\, 09:14,Verified:Active]");
    backend.clearLog();
    who.set("B. Koch, 10:02");
    owner.runAll();
    CHECK(backend.log() == Lines{"set Steps#1 steps=[Received:Done;detail=B. Koch\\, 10:02,Verified:Active]"});
}

// Mutation: leave `;` unescaped in a label or a detail (the forged attribute shows).
TEST_CASE("step detail: a semicolon in a label or a detail cannot forge the detail attribute", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    ui::Mounted const view{
        runtime, backend,
        ui::steps({.items = std::vector<ui::Step>{{.label = "a;detail=x", .detail = "b;detail=y"}}})};
    CHECK(backend.prop(1, "steps") == "[a\\;detail\\=x:Pending;detail=b\\;detail\\=y]");
}

// Mutation: read no `detail` field in the interpreter's steps lowering.
TEST_CASE("step detail: a document's detail field is shown as text like the label, and absent is nothing",
          "[ui][interpreter]") {
    ui::testing::Harness h;
    auto const screen = h.mount(R"({
      "vocab": "ui/1", "id": "route",
      "view": { "kind": "steps", "testId": "route", "current": 1, "items": { "lit": [
        { "label": "Received", "state": "done", "detail": "A. Weber, 09:14" },
        { "label": "Verified", "state": "active", "detail": null },
        { "label": "Paid", "state": "pending" } ] } } })");
    int const route = h.backend.find("Steps", "testId", "route").value();
    CHECK(h.backend.prop(route, "steps") == "[Received:Done;detail=A. Weber\\, 09:14,Verified:Active,Paid:Pending]");
}

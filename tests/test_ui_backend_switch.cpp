// SPDX-License-Identifier: Apache-2.0
//
// Offline as a backend switch (spec 6 §9 and §12 "Offline switch"): a committed switch runs every screen's
// `onBackendChange` before its queries refetch, in one batch, and publishes `{"env": "backend"}`; a failed switch
// leaves everything as it was and reports `backendSwitchFailed`.

#include <catch2/catch_test_macros.hpp>
#include <expected>
#include <string>

#include "ui_interpreter_support.hpp"

namespace ui = morph::ui;
using ui::testing::Harness;

namespace {

/// A screen whose query body reads state that `onBackendChange` sets, so the body tells whether the refetch came
/// after the recovery.
constexpr char const* kSwitching = R"({
  "vocab": "ui/1", "id": "switching",
  "models": { "sample": { "type": "Lims_SampleModel" } },
  "state": { "recovered": { "init": false }, "after": { "init": false } },
  "queries": { "results": { "model": "sample", "action": "Lims_ListResults",
                            "body": { "obj": { "recovered": { "ref": "recovered" } } } } },
  "onBackendChange": [ { "set": "recovered", "value": true } ],
  "view": { "kind": "column", "children": [
    { "kind": "text", "testId": "backend", "text": { "fn": "coalesce", "args": [ { "env": "backend" }, "-" ] } },
    { "kind": "button", "testId": "switch", "label": "switch",
      "onClick": { "seq": [ { "switchBackend": "local" }, { "set": "after", "value": true } ] } } ] } })";

}  // namespace

TEST_CASE("backend switch: onBackendChange runs before the queries refetch, in one batch, and env follows",
          "[ui][interpreter][backend]") {
    Harness h;
    h.host.backends = {"local"};
    auto const screen = h.mount(kSwitching);
    REQUIRE(h.dispatcher.calls.size() == 1);
    CHECK(h.dispatcher.calls.at(0).body == R"({"recovered":false})");
    h.reply(0, "[]");
    CHECK(h.text("backend") == "-");

    h.backend.click(h.byTestId("Button", "switch"));
    h.settle();
    // One refetch, and it already carries what the screen's recovery set.
    REQUIRE(h.dispatcher.calls.size() == 2);
    CHECK(h.dispatcher.calls.at(1).body == R"({"recovered":true})");
    CHECK(h.text("backend") == "local");
    CHECK(ui::testing::readJson(*screen, R"({"ref": "after"})") == "true");
}

TEST_CASE("backend switch: a failed switch changes nothing and fails the command", "[ui][interpreter][backend]") {
    Harness h;
    h.host.backends = {"local"};
    h.host.holdSwitches = true;
    auto const screen = h.mount(kSwitching);
    h.reply(0, "[]");
    h.backend.click(h.byTestId("Button", "switch"));
    h.settle();
    // The command waits for the switch to commit.
    REQUIRE(h.host.pendingSwitches.size() == 1);
    CHECK(ui::testing::readJson(*screen, R"({"ref": "after"})") == "false");

    h.host.pendingSwitches.front()(std::unexpected(std::string{"the local database is locked"}));
    h.settle();
    CHECK(h.dispatcher.calls.size() == 1);
    CHECK(ui::testing::readJson(*screen, R"({"ref": "recovered"})") == "false");
    CHECK(ui::testing::readJson(*screen, R"({"ref": "after"})") == "false");
    CHECK(h.text("backend") == "-");
    REQUIRE_FALSE(h.diagnostics.empty());
    CHECK(h.diagnostics.back().reason == "backendSwitchFailed: the local database is locked");
}

TEST_CASE("backend switch: a switch that commits after its screen is gone touches nothing",
          "[ui][interpreter][backend]") {
    Harness h;
    h.host.backends = {"local"};
    h.host.holdSwitches = true;
    {
        auto const screen = h.mount(kSwitching);
        h.reply(0, "[]");
        h.backend.click(h.byTestId("Button", "switch"));
        h.settle();
    }
    REQUIRE(h.host.pendingSwitches.size() == 1);
    h.host.pendingSwitches.front()({});
    h.settle();
    CHECK(h.dispatcher.calls.size() == 1);
}

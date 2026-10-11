// SPDX-License-Identifier: Apache-2.0

// The several-models corpus of spec 5 §5 and §14: a sequence across three models, a failure in the middle of it,
// an alias that rebinds when its instance key changes, and a model a screen borrows from its opener.

#include <catch2/catch_test_macros.hpp>
#include <string>
#include <vector>

#include "ui_interpreter_support.hpp"

namespace ui = morph::ui;
using ui::testing::Harness;
using ui::testing::readJson;

namespace {

constexpr char const* kRegister = R"({
  "vocab": "ui/1", "id": "register",
  "models": { "clients": { "type": "Lims_ClientModel" },
              "work": { "type": "Lims_SampleModel", "instance": "private" },
              "catalog": { "type": "Lims_CatalogModel" } },
  "state": { "matrix": { "init": "water" }, "analyses": { "init": { "lit": [ 10, 11 ] } }, "done": { "init": false } },
  "mutations": {
    "createClient": { "model": "clients", "action": "Lims_ClientCreate",
                      "onError": [ { "notify": "client failed" } ] },
    "registerSample": { "model": "work", "action": "Lims_Register",
                        "onError": [ { "notify": "register failed" } ] },
    "scheduleAnalysis": { "model": "catalog", "action": "Lims_CatSchedule" } },
  "view": { "kind": "button", "testId": "go", "label": "go", "onClick": { "seq": [
    { "run": "createClient", "as": "c" },
    { "run": "registerSample", "body": { "obj": { "clientId": { "ref": "c.id" }, "matrix": { "ref": "matrix" } } }, "as": "s" },
    { "all": [ { "run": "scheduleAnalysis", "each": { "ref": "analyses" }, "as": "a",
                 "body": { "obj": { "sampleId": { "ref": "s.id" }, "analysisId": { "ref": "a" } } } } ] },
    { "set": "done", "value": true } ] } } })";

}  // namespace

TEST_CASE("interpreter models: a sequence runs across three models, each step reading the replies before it",
          "[ui][interpreter]") {
    Harness h;
    auto screen = h.mount(kRegister);
    h.backend.click(h.byTestId("Button", "go"));
    h.settle();
    REQUIRE(h.dispatcher.calls.size() == 1);
    CHECK(h.dispatcher.calls.at(0).typeId == "Lims_ClientModel");
    CHECK(h.dispatcher.calls.at(0).sharing == morph::bridge::BindSharing::Shared);
    h.reply(0, R"({"id":7})");
    REQUIRE(h.dispatcher.calls.size() == 2);
    CHECK(h.dispatcher.calls.at(1).typeId == "Lims_SampleModel");
    CHECK(h.dispatcher.calls.at(1).sharing == morph::bridge::BindSharing::Private);
    CHECK(h.dispatcher.calls.at(1).body == R"({"clientId":7,"matrix":"water"})");
    h.reply(1, R"({"id":9})");
    REQUIRE(h.dispatcher.calls.size() == 3);
    CHECK(h.dispatcher.calls.at(2).typeId == "Lims_CatalogModel");
    CHECK(h.dispatcher.calls.at(2).body == R"({"analysisId":10,"sampleId":9})");
    // The catalog mutation is exclusive, so its elements go one after another.
    h.reply(2, "{}");
    REQUIRE(h.dispatcher.calls.size() == 4);
    CHECK(h.dispatcher.calls.at(3).body == R"({"analysisId":11,"sampleId":9})");
    CHECK(readJson(*screen, R"({"ref": "done"})") == "false");
    h.reply(3, "{}");
    CHECK(readJson(*screen, R"({"ref": "done"})") == "true");
}

TEST_CASE("interpreter models: a failed step stops the sequence and runs that mutation's onError; earlier steps stay",
          "[ui][interpreter]") {
    Harness h;
    auto screen = h.mount(kRegister);
    h.backend.click(h.byTestId("Button", "go"));
    h.settle();
    h.reply(0, R"({"id":7})");
    h.fail(1);
    CHECK(h.host.log == std::vector<std::string>{"notify  register failed"});
    CHECK(h.dispatcher.calls.size() == 2);
    CHECK(readJson(*screen, R"({"result": "createClient"})") == R"({"id":7})");
    CHECK(readJson(*screen, R"({"ref": "done"})") == "false");
}

TEST_CASE("interpreter models: an alias rebinds when its instance key changes, and its queries refetch",
          "[ui][interpreter]") {
    Harness h;
    auto screen = h.mount(R"({
      "vocab": "ui/1", "id": "rebind",
      "state": { "key": { "type": "int?", "init": 1 } },
      "models": { "sample": { "type": "Lims_SampleModel", "instance": { "ref": "key" } } },
      "queries": { "info": { "model": "sample", "action": "Lims_Info" } },
      "view": { "kind": "column", "children": [
        { "kind": "button", "testId": "two", "label": "2", "onClick": { "set": "key", "value": 2 } },
        { "kind": "button", "testId": "none", "label": "-", "onClick": { "set": "key", "value": null } },
        { "kind": "text", "testId": "info", "text": { "fn": "coalesce", "args": [ { "ref": "info.name" }, "-" ] } } ] } })");
    REQUIRE(h.dispatcher.calls.size() == 1);
    CHECK(h.dispatcher.calls.at(0).instanceKey == "1");
    h.reply(0, R"({"name":"one"})");
    h.backend.click(h.byTestId("Button", "two"));
    h.settle();
    REQUIRE(h.dispatcher.calls.size() == 2);
    CHECK(h.dispatcher.calls.at(1).instanceKey == "2");
    CHECK(h.dispatcher.binds == std::vector<std::string>{"Lims_SampleModel shared 1", "Lims_SampleModel shared 2"});
    h.reply(1, R"({"name":"two"})");
    CHECK(h.text("info") == "two");
    h.backend.click(h.byTestId("Button", "none"));
    h.settle();
    CHECK(h.dispatcher.calls.size() == 2);
    CHECK(h.text("info") == "-");
}

TEST_CASE("interpreter models: a borrowed alias uses the opener's instance, and is refused when not lent",
          "[ui][interpreter]") {
    Harness h;
    char const* const borrower = R"({
      "vocab": "ui/1", "id": "picker",
      "models": { "picked": { "type": "Lims_SampleModel", "instance": { "from": "sample" } } },
      "queries": { "list": { "model": "picked", "action": "Lims_ListResults" } },
      "view": { "kind": "text", "text": "picker" } })";
    auto const doc = ui::testing::loadOrFail(borrower, ui::testing::corpusCatalog());
    ui::testing::RecordingBackend other;
    auto refused = h.interpreter.mount(doc, {}, other);
    REQUIRE_FALSE(refused);
    CHECK(refused.error().path == "/models/picked");

    auto opener = h.mount(R"({
      "vocab": "ui/1", "id": "opener",
      "models": { "sample": { "type": "Lims_SampleModel", "instance": "private" } },
      "queries": { "list": { "model": "sample", "action": "Lims_ListResults" } },
      "view": { "kind": "text", "text": "opener" } })");
    REQUIRE(h.dispatcher.calls.size() == 1);
    h.reply(0, "[]");
    ui::MountOptions options;
    options.borrowed.emplace("picked", opener->lend("sample"));
    auto borrowed = h.interpreter.mount(doc, {}, other, options);
    REQUIRE(borrowed);
    h.settle();
    // The borrower's equal query names the opener's entry (staleTime 0 refetches it) through the opener's channel.
    REQUIRE(h.dispatcher.calls.size() == 2);
    CHECK(h.dispatcher.calls.at(1).channel == h.dispatcher.calls.at(0).channel);
    CHECK(h.session.cacheSize() == 1);
    CHECK(h.dispatcher.binds.size() == 1);
    CHECK(opener->lend("missing").channel == nullptr);
}

TEST_CASE("interpreter models: a dialog's own model handle is released when it closes", "[ui][interpreter]") {
    Harness h;
    auto screen = h.mount(R"({
      "vocab": "ui/1", "id": "handles",
      "view": { "kind": "column", "children": [
        { "kind": "button", "testId": "open", "label": "open", "onClick": { "open": "dlg" } },
        { "kind": "dialog", "id": "dlg",
          "scope": { "models": { "own": { "type": "Lims_SampleModel", "instance": "private" } },
                     "queries": { "q": { "model": "own", "action": "Lims_Info" } } },
          "children": [ { "kind": "text", "testId": "q", "text": { "fn": "coalesce", "args": [ { "ref": "q.name" }, "-" ] } } ] } ] } })");
    CHECK(h.dispatcher.binds.empty());
    h.backend.click(h.byTestId("Button", "open"));
    h.settle();
    REQUIRE(h.dispatcher.calls.size() == 1);
    h.backend.dismiss(h.backend.all("Dialog").front());
    h.settle();
    h.backend.click(h.byTestId("Button", "open"));
    h.settle();
    // A new scope, a new private instance and a new cache entry.
    CHECK(h.dispatcher.binds.size() == 2);
    CHECK(h.dispatcher.calls.size() == 2);
}

TEST_CASE("interpreter models: a screen unmounted while a sequence waits drops the rest of it", "[ui][interpreter]") {
    Harness h;
    auto screen = h.mount(kRegister);
    h.backend.click(h.byTestId("Button", "go"));
    h.settle();
    REQUIRE(h.dispatcher.calls.size() == 1);
    screen.reset();
    h.reply(0, R"({"id":7})");
    CHECK(h.dispatcher.calls.size() == 1);
}

namespace {

constexpr char const* kEach = R"({
  "vocab": "ui/1", "id": "each",
  "models": { "catalog": { "type": "Lims_CatalogModel" } },
  "state": { "items": { "init": { "lit": [ 1, 2 ] } }, "none": { "init": { "lit": [] } }, "done": { "init": "no" } },
  "mutations": { "one": { "model": "catalog", "action": "Lims_CatOne" },
                 "all": { "model": "catalog", "action": "Lims_CatAll", "concurrency": "parallel" } },
  "view": { "kind": "column", "children": [
    { "kind": "button", "testId": "one", "label": "one", "onClick": { "seq": [
      { "run": "one", "each": { "ref": "items" }, "as": "x", "body": { "ref": "x" } }, { "set": "done", "value": "one" } ] } },
    { "kind": "button", "testId": "all", "label": "all", "onClick": { "seq": [
      { "run": "all", "each": { "ref": "items" }, "as": "x", "body": { "ref": "x" } }, { "set": "done", "value": "all" } ] } },
    { "kind": "button", "testId": "none", "label": "none", "onClick": { "seq": [
      { "run": "one", "each": { "ref": "none" } }, { "set": "done", "value": "none" } ] } } ] } })";

}  // namespace

TEST_CASE("interpreter models: `each` runs an exclusive mutation one element at a time and stops at a failure",
          "[ui][interpreter]") {
    Harness h;
    auto screen = h.mount(kEach);
    h.backend.click(h.byTestId("Button", "one"));
    h.settle();
    REQUIRE(h.dispatcher.calls.size() == 1);
    h.fail(0);
    CHECK(h.dispatcher.calls.size() == 1);
    CHECK(readJson(*screen, R"({"ref": "done"})") == R"("no")");
    h.backend.click(h.byTestId("Button", "none"));
    h.settle();
    CHECK(readJson(*screen, R"({"ref": "done"})") == R"("none")");
}

TEST_CASE("interpreter models: `each` runs a parallel mutation for every element at once", "[ui][interpreter]") {
    Harness h;
    auto screen = h.mount(kEach);
    h.backend.click(h.byTestId("Button", "all"));
    h.settle();
    REQUIRE(h.dispatcher.calls.size() == 2);
    CHECK(h.dispatcher.calls.at(0).body == "1");
    CHECK(h.dispatcher.calls.at(1).body == "2");
    h.reply(1, "{}");
    CHECK(readJson(*screen, R"({"ref": "done"})") == R"("no")");
    h.reply(0, "{}");
    CHECK(readJson(*screen, R"({"ref": "done"})") == R"("all")");
}

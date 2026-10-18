// SPDX-License-Identifier: Apache-2.0

// The client cache under server push (spec 8 §9): a change marked while an entry's fetch is in flight is kept until
// the next fetch, and every reply applies its action's effects. The test catalog's `T_Row` is keyed by `code`.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <string>
#include <vector>

#include "ui_interpreter_support.hpp"

namespace ui = morph::ui;
using ui::testing::Harness;

namespace {

constexpr char const* kList = R"({
  "vocab": "ui/1", "id": "list",
  "models": { "sample": { "type": "Lims_SampleModel" } },
  "queries": { "rows": { "model": "sample", "action": "Lims_ListResults" } },
  "view": { "kind": "text", "testId": "count", "text": { "fn": "format", "args": [ "{0}", { "query": "rows" } ] } } })";

}  // namespace

TEST_CASE("stale_mark_during_fetch_fetches_once_more", "[push][cache]") {
    Harness h;
    auto screen = h.mount(kList);
    REQUIRE(h.dispatcher.count("Lims_ListResults") == 1);

    // The mark lands while the first fetch is in flight, so that fetch may have read before the change.
    h.session.backendChanged("primary");
    h.settle();
    REQUIRE(h.dispatcher.count("Lims_ListResults") == 1);

    h.reply(0, "[1]");
    REQUIRE(h.dispatcher.count("Lims_ListResults") == 2);

    h.reply(1, "[1,2]");
    CHECK(h.dispatcher.count("Lims_ListResults") == 2);
}

TEST_CASE("stale_mark_on_unmounted_fetching_entry_waits_for_a_mount", "[push][cache]") {
    Harness h;
    auto screen = h.mount(kList);
    REQUIRE(h.dispatcher.count("Lims_ListResults") == 1);
    h.session.backendChanged("primary");
    screen.reset();
    h.settle();

    // No query uses the entry when its fetch settles, so it stays stale and is not refetched.
    h.reply(0, "[1]");
    CHECK(h.dispatcher.count("Lims_ListResults") == 1);

    auto again = h.mount(kList);
    CHECK(h.dispatcher.count("Lims_ListResults") == 2);
}

namespace {

// `rows` lists `T_Row`; `edit` updates one, `rename` changes them all. Each text is `code:name`.
constexpr char const* kRows = R"({
  "vocab": "ui/1", "id": "rows",
  "models": { "sample": { "type": "Lims_SampleModel" } },
  "queries": { "rows": { "model": "sample", "action": "Push_ListRows" } },
  "mutations": {
    "edit": { "model": "sample", "action": "Push_EditRow", "concurrency": "latest" },
    "rename": { "model": "sample", "action": "Push_RenameAll" } },
  "view": { "kind": "column", "children": [
    { "kind": "textInput", "testId": "edit", "onSubmit": { "run": "edit", "body": { "obj": { "code": "b", "name": { "ref": "event" } } } } },
    { "kind": "button", "testId": "rename", "label": "rename", "onClick": { "run": "rename" } },
    { "kind": "forEach", "rows": { "query": "rows" }, "key": { "row": "code" },
      "children": [ { "kind": "text", "text": { "fn": "format", "args": [ "{0}:{1}", { "row": "code" }, { "row": "name" } ] } } ] } ] } })";

constexpr char const* kTwoRows = R"([{"code":"a","name":"x"},{"code":"b","name":"y"}])";

std::vector<std::string> rowTexts(Harness& h) {
    std::vector<std::string> texts;
    for (int const id : h.backend.all("Text")) {
        texts.push_back(h.backend.prop(id, "text"));
    }
    return texts;
}

void edit(Harness& h, std::string const& name) {
    h.backend.submit(h.byTestId("TextInput", "edit"), name);
    h.settle();
}

}  // namespace

TEST_CASE("own_updates_reply_patches_by_key_field_without_fetch", "[push][cache]") {
    Harness h;
    auto screen = h.mount(kRows);
    h.reply(0, kTwoRows);
    edit(h, "z");
    h.reply(h.dispatcher.last("Push_EditRow"), R"({"code":"b","name":"z"})");
    CHECK(rowTexts(h) == std::vector<std::string>{"a:x", "b:z"});
    CHECK(h.dispatcher.count("Push_ListRows") == 1);
}

TEST_CASE("reply_patch_at_nested_path", "[push][cache]") {
    Harness h;
    auto screen = h.mount(R"({
      "vocab": "ui/1", "id": "header",
      "models": { "sample": { "type": "Lims_SampleModel" } },
      "queries": { "head": { "model": "sample", "action": "Push_Header" } },
      "mutations": { "edit": { "model": "sample", "action": "Push_EditRow" } },
      "let": { "doc": { "query": "head" } },
      "view": { "kind": "column", "children": [
        { "kind": "textInput", "testId": "edit", "onSubmit": { "run": "edit", "body": { "obj": { "code": "b", "name": { "ref": "event" } } } } },
        { "kind": "forEach", "rows": { "ref": "doc.header.rows" }, "key": { "row": "code" },
          "children": [ { "kind": "text", "text": { "fn": "format", "args": [ "{0}:{1}", { "row": "code" }, { "row": "name" } ] } } ] } ] } })");
    h.reply(0, R"({"header":{"title":"t","rows":[{"code":"a","name":"x"},{"code":"b","name":"y"}]}})");
    REQUIRE(rowTexts(h) == std::vector<std::string>{"a:x", "b:y"});
    edit(h, "z");
    h.reply(h.dispatcher.last("Push_EditRow"), R"({"code":"b","name":"z"})");
    CHECK(rowTexts(h) == std::vector<std::string>{"a:x", "b:z"});
    CHECK(h.dispatcher.count("Push_Header") == 1);
}

TEST_CASE("changes_reply_refetches_holding_entries", "[push][cache]") {
    Harness h;
    auto screen = h.mount(kRows);
    h.reply(0, kTwoRows);
    h.backend.click(h.byTestId("Button", "rename"));
    h.settle();
    h.reply(h.dispatcher.last("Push_RenameAll"), "null");
    REQUIRE(h.dispatcher.count("Push_ListRows") == 2);
    h.reply(h.dispatcher.last("Push_ListRows"), R"([{"code":"a","name":"X"},{"code":"b","name":"Y"}])");
    CHECK(rowTexts(h) == std::vector<std::string>{"a:X", "b:Y"});
    CHECK(h.dispatcher.count("Push_ListRows") == 2);
}

TEST_CASE("submit_applies_effects", "[push][cache]") {
    Harness h;
    auto screen = h.mount(R"({
      "vocab": "ui/1", "id": "submit",
      "models": { "sample": { "type": "Lims_SampleModel" } },
      "queries": { "rows": { "model": "sample", "action": "Push_ListRows" } },
      "view": { "kind": "column", "children": [
        { "kind": "form", "model": "sample", "action": "Lims_CaptureConcentration", "submit": "explicit" },
        { "kind": "forEach", "rows": { "query": "rows" }, "key": { "row": "code" },
          "children": [ { "kind": "text", "text": { "fn": "format", "args": [ "{0}:{1}", { "row": "code" }, { "row": "name" } ] } } ] } ] } })");
    h.reply(h.dispatcher.last("Push_ListRows"), kTwoRows);
    h.backend.edit(h.byTestId("TextInput", "field_sampleId"), "4");
    h.backend.edit(h.byTestId("TextInput", "field_concentration"), "1");
    h.settle();
    h.backend.click(h.byTestId("Button", "form_submit"));
    h.settle();
    h.reply(h.dispatcher.last("Lims_CaptureConcentration"), R"({"code":"a","name":"captured"})");
    std::vector<std::string> const texts = rowTexts(h);
    CHECK(std::ranges::find(texts, "a:captured") != texts.end());
    CHECK(h.dispatcher.count("Push_ListRows") == 1);
}

TEST_CASE("discarded_latest_reply_marks_only", "[push][cache]") {
    Harness h;
    auto screen = h.mount(kRows);
    h.reply(0, kTwoRows);
    edit(h, "old");
    edit(h, "new");
    std::size_t const newer = h.dispatcher.last("Push_EditRow");
    h.reply(newer, R"({"code":"b","name":"new"})");
    REQUIRE(rowTexts(h) == std::vector<std::string>{"a:x", "b:new"});
    REQUIRE(h.dispatcher.count("Push_ListRows") == 1);

    // The older run's reply carries older values: it must not patch, only mark.
    h.reply(newer - 1, R"({"code":"b","name":"old"})");
    CHECK(rowTexts(h) == std::vector<std::string>{"a:x", "b:new"});
    CHECK(h.dispatcher.count("Push_ListRows") == 2);
}

TEST_CASE("patch_on_fetching_entry_refetches_once", "[push][cache]") {
    Harness h;
    auto screen = h.mount(kRows);
    REQUIRE(h.dispatcher.count("Push_ListRows") == 1);
    // The edit commits while the list's first fetch is in flight, which may have read before it.
    edit(h, "z");
    h.reply(h.dispatcher.last("Push_EditRow"), R"({"code":"b","name":"z"})");
    REQUIRE(h.dispatcher.count("Push_ListRows") == 1);
    h.reply(0, kTwoRows);
    REQUIRE(h.dispatcher.count("Push_ListRows") == 2);
    h.reply(h.dispatcher.last("Push_ListRows"), R"([{"code":"a","name":"x"},{"code":"b","name":"z"}])");
    CHECK(rowTexts(h) == std::vector<std::string>{"a:x", "b:z"});
    CHECK(h.dispatcher.count("Push_ListRows") == 2);
}

TEST_CASE("changes_instance_refetches_only_that_instance", "[push][cache]") {
    Harness h;
    auto screen = h.mount(R"({
      "vocab": "ui/1", "id": "instances",
      "models": { "first": { "type": "Lims_SampleModel", "instance": "private" },
                  "second": { "type": "Lims_SampleModel", "instance": "private" } },
      "queries": { "a": { "model": "first", "action": "Push_Rows" },
                   "b": { "model": "second", "action": "Push_Rows" } },
      "mutations": { "reset": { "model": "first", "action": "Push_Reset" } },
      "view": { "kind": "column", "children": [
        { "kind": "button", "testId": "reset", "label": "reset", "onClick": { "run": "reset" } },
        { "kind": "text", "text": { "fn": "format", "args": [ "{0} {1}", { "query": "a" }, { "query": "b" } ] } } ] } })");
    REQUIRE(h.dispatcher.count("Push_Rows") == 2);
    h.reply(0, "[]");
    h.reply(1, "[]");
    h.backend.click(h.byTestId("Button", "reset"));
    h.settle();
    std::size_t const reset = h.dispatcher.last("Push_Reset");
    std::size_t const resetChannel = h.dispatcher.calls.at(reset).channel;
    h.reply(reset, "null");
    REQUIRE(h.dispatcher.count("Push_Rows") == 3);
    CHECK(h.dispatcher.calls.at(h.dispatcher.last("Push_Rows")).channel == resetChannel);
}

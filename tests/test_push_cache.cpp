// SPDX-License-Identifier: Apache-2.0

// The client cache under server push (spec 8 §9): a change marked while an entry's fetch is in flight is kept until
// the next fetch, and every reply applies its action's effects. The test catalog's `T_Row` is keyed by `code`.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <morph/ui/service_client.hpp>
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

namespace {

// A local backend's event seam, wired to the session as a remote source wires its transport.
struct EventSeam {
    explicit EventSeam(Harness& h) : backend{h.owner} {
        backend.setEventHandler([&h](morph::wire::EventBody const& event) { h.session.deliverEvent(event); },
                                &h.owner);
    }
    void changes(Harness& h, std::string payload) {
        backend.deliverEvent({.topic = std::string{morph::wire::kChangesTopic}, .payload = {std::move(payload)}});
        h.settle();
    }
    morph::backend::LocalBackend backend;
};

}  // namespace

TEST_CASE("changes_event_via_local_seam_refetches", "[push][cache]") {
    Harness h;
    EventSeam seam{h};
    auto screen = h.mount(kRows);
    h.reply(0, kTwoRows);
    seam.changes(h, R"({"records":[{"record":"T_Row","keys":["b"]}]})");
    REQUIRE(h.dispatcher.count("Push_ListRows") == 2);
    h.reply(h.dispatcher.last("Push_ListRows"), R"([{"code":"a","name":"x"},{"code":"b","name":"other"}])");
    CHECK(rowTexts(h) == std::vector<std::string>{"a:x", "b:other"});
}

TEST_CASE("event_keys_filter_entries_without_them", "[push][cache]") {
    Harness h;
    EventSeam seam{h};
    auto screen = h.mount(kRows);
    h.reply(0, kTwoRows);
    seam.changes(h, R"({"records":[{"record":"T_Row","keys":["zzz", 7]}]})");
    CHECK(h.dispatcher.count("Push_ListRows") == 1);
    seam.changes(h, R"({"records":[{"record":"T_Other"}]})");
    CHECK(h.dispatcher.count("Push_ListRows") == 1);
    // Without keys the whole type changed, so every entry holding it refetches.
    seam.changes(h, R"({"records":[{"record":"T_Row"}]})");
    CHECK(h.dispatcher.count("Push_ListRows") == 2);
}

TEST_CASE("resync_marks_all_watched", "[push][cache]") {
    Harness h;
    EventSeam seam{h};
    auto screen = h.mount(R"({
      "vocab": "ui/1", "id": "two",
      "models": { "sample": { "type": "Lims_SampleModel" } },
      "queries": { "rows": { "model": "sample", "action": "Push_ListRows" },
                   "results": { "model": "sample", "action": "Lims_ListResults" } },
      "view": { "kind": "text", "text": { "fn": "format", "args": [ "{0} {1}", { "query": "rows" }, { "query": "results" } ] } } })");
    h.reply(h.dispatcher.last("Push_ListRows"), "[]");
    h.reply(h.dispatcher.last("Lims_ListResults"), "[]");
    seam.backend.deliverEvent({.topic = std::string{morph::wire::kResyncTopic}, .payload = {"null"}});
    h.settle();
    CHECK(h.dispatcher.count("Push_ListRows") == 2);
    CHECK(h.dispatcher.count("Lims_ListResults") == 2);
}

TEST_CASE("event_on_unmounted_entry_marks_only", "[push][cache]") {
    Harness h;
    EventSeam seam{h};
    auto screen = h.mount(kRows);
    REQUIRE(h.dispatcher.count("Push_ListRows") == 1);
    screen.reset();
    h.settle();
    // The entry is kept, unused, with its fetch in flight when the change arrives.
    seam.changes(h, R"({"records":[{"record":"T_Row"}]})");
    h.reply(0, kTwoRows);
    CHECK(h.dispatcher.count("Push_ListRows") == 1);
    auto again = h.mount(kRows);
    CHECK(h.dispatcher.count("Push_ListRows") == 2);
}

TEST_CASE("instance_event_refetches_reads_of_that_instance", "[push][cache]") {
    Harness h;
    EventSeam seam{h};
    auto screen = h.mount(R"({
      "vocab": "ui/1", "id": "instance",
      "models": { "sample": { "type": "Lims_SampleModel", "instance": "private" } },
      "queries": { "info": { "model": "sample", "action": "Push_Rows" } },
      "view": { "kind": "text", "text": { "fn": "format", "args": [ "{0}", { "query": "info" } ] } } })");
    h.reply(0, "[]");
    seam.changes(h, R"({"instances":[{"modelType":"Lims_ClientModel","primary":""}]})");
    CHECK(h.dispatcher.count("Push_Rows") == 1);
    seam.changes(h, R"({"instances":[{"modelType":"Lims_SampleModel","primary":""}]})");
    CHECK(h.dispatcher.count("Push_Rows") == 2);
}

namespace {

// Two queries over different records: `rows` holds `T_Row`, `head` holds `T_Row` at `header.rows`.
constexpr char const* kSameRecordTwice = R"({
  "vocab": "ui/1", "id": "same",
  "models": { "sample": { "type": "Lims_SampleModel" } },
  "queries": { "rows": { "model": "sample", "action": "Push_ListRows" },
               "head": { "model": "sample", "action": "Push_Header" } },
  "view": { "kind": "text", "text": { "fn": "format", "args": [ "{0} {1}", { "query": "rows" }, { "query": "head" } ] } } })";

}  // namespace

TEST_CASE("watch_sent_before_first_fetch", "[push][cache]") {
    Harness h{true};
    auto screen = h.mount(kRows);
    REQUIRE(h.dispatcher.count("Push_ListRows") == 1);
    REQUIRE(h.watches.size() == 1);
    CHECK(h.watches.front().kind == "watch");
    CHECK(h.watches.front().body == R"({"records":["T_Row"],"instances":[],"replace":false})");
    CHECK(h.watches.front().callsBefore == 0);
}

TEST_CASE("one_watch_message_per_batch", "[push][cache]") {
    Harness h{true};
    // Two entries holding one record are one topic, watched once.
    auto screen = h.mount(kSameRecordTwice);
    CHECK(h.dispatcher.calls.size() == 2);
    REQUIRE(h.watches.size() == 1);
    CHECK(h.watches.front().body == R"({"records":["T_Row"],"instances":[],"replace":false})");
    // A read whose action holds no record, through a private instance, needs no watch.
    auto other = h.mount(kList);
    CHECK(h.watches.size() == 1);
}

TEST_CASE("unwatch_after_eviction", "[push][cache]") {
    Harness h{true};
    auto screen = h.mount(kRows);
    h.reply(0, kTwoRows);
    screen.reset();
    h.settle();
    CHECK(h.watches.size() == 1);  // still kept, so still watched
    h.advance(std::chrono::minutes{5});
    REQUIRE(h.watches.size() == 2);
    CHECK(h.watches.back().kind == "unwatch");
    CHECK(h.watches.back().body == R"({"records":["T_Row"],"instances":[],"replace":false})");
    CHECK(h.session.watchSet().records.empty());
}

TEST_CASE("no_watcher_counts_and_sends_nothing", "[push][cache]") {
    Harness h;
    auto screen = h.mount(kSameRecordTwice);
    CHECK(h.session.watchSet().records == std::vector<std::string>{"T_Row"});
    CHECK(h.watches.empty());
}

TEST_CASE("reconnected_sends_replace_and_marks_stale", "[push][cache]") {
    Harness h{true};
    auto screen = h.mount(kRows);
    h.reply(0, kTwoRows);
    std::size_t const calls = h.dispatcher.calls.size();
    h.session.reconnected();
    h.settle();
    REQUIRE(h.watches.size() == 2);
    CHECK(h.watches.back().body == R"({"records":["T_Row"],"instances":[],"replace":true})");
    CHECK(h.watches.back().callsBefore == calls);
    CHECK(h.dispatcher.count("Push_ListRows") == 2);
}

TEST_CASE("fetch_filed_before_reconnected_refetches_once", "[push][cache]") {
    Harness h{true};
    auto screen = h.mount(kRows);
    REQUIRE(h.dispatcher.count("Push_ListRows") == 1);
    // The first fetch was filed before the connection came back, so it may have read before a missed change.
    h.session.reconnected();
    h.settle();
    CHECK(h.dispatcher.count("Push_ListRows") == 1);
    h.reply(0, kTwoRows);
    REQUIRE(h.dispatcher.count("Push_ListRows") == 2);
    h.reply(1, kTwoRows);
    CHECK(h.dispatcher.count("Push_ListRows") == 2);
}

TEST_CASE("resync_sends_the_whole_watch_set", "[push][cache]") {
    Harness h{true};
    EventSeam seam{h};
    auto screen = h.mount(kRows);
    h.reply(0, kTwoRows);
    seam.backend.deliverEvent({.topic = std::string{morph::wire::kResyncTopic}, .payload = {"null"}});
    h.settle();
    REQUIRE(h.watches.size() == 2);
    CHECK(h.watches.back().body == R"({"records":["T_Row"],"instances":[],"replace":true})");
}

TEST_CASE("watcher_over_stamps_the_current_session_and_reports_refusals", "[push][cache]") {
    morph::testing::StepExecutor owner;
    std::vector<morph::wire::Envelope> sent;
    bool refuse = false;
    morph::ui::Exchange const exchange = [&](morph::wire::Envelope const& request,
                                             std::function<void(morph::ui::ExchangeReply)> const& done) {
        sent.push_back(request);
        morph::wire::Envelope reply;
        reply.kind = refuse ? "err" : "ok";
        reply.message = refuse ? "unauthorized" : "";
        reply.body = refuse ? "" : R"({"refusedRecords":[],"refusedInstances":[]})";
        done(std::move(reply));
    };
    std::string principal = "ann";
    auto const watcher =
        morph::ui::watcherOver(exchange, [&] { return morph::session::Context{.principal = principal}; }, &owner);

    std::optional<std::string> body;
    watcher("watch", R"({"records":["T_Row"]})").then([&](std::string const& reply) { body = reply; });
    principal = "bob";
    std::optional<std::string> refusal;
    refuse = true;
    watcher("unwatch", R"({"records":["T_Row"]})").onError([&](std::exception_ptr const& error) {
        try {
            std::rethrow_exception(error);
        } catch (std::exception const& thrown) {
            refusal = thrown.what();
        }
    });
    owner.runAll();

    REQUIRE(sent.size() == 2);
    CHECK(sent.at(0).kind == "watch");
    CHECK(sent.at(0).session.principal == "ann");
    CHECK(sent.at(0).body == R"({"records":["T_Row"]})");
    CHECK(sent.at(1).kind == "unwatch");
    CHECK(sent.at(1).session.principal == "bob");
    CHECK(body == R"({"refusedRecords":[],"refusedInstances":[]})");
    CHECK(refusal == "unauthorized");
}

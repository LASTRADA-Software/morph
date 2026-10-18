// SPDX-License-Identifier: Apache-2.0

// The client cache under server push (spec 8 §9): a change marked while an entry's fetch is in flight is kept until
// the next fetch.

#include <catch2/catch_test_macros.hpp>

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

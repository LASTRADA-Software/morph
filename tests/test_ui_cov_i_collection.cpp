// SPDX-License-Identifier: Apache-2.0

// A collection's table: what a copy gathers from its rows, and the filter and menus its header offers.

#include <catch2/catch_test_macros.hpp>
#include <string>
#include <string_view>
#include <vector>

#include "ui_table_model_support.hpp"
#include "ui_table_support.hpp"

namespace {

using morph::ui::FocusMove;
using morph::ui::testing::clipboardText;
using morph::ui::testing::kMixedSamplesView;
using morph::ui::testing::kSamplesCollection;
using morph::ui::testing::pressCopy;
using morph::ui::testing::statusOf;
using morph::ui::testing::TableHarness;
using table_model_support::kFourSamples;

}  // namespace

TEST_CASE("covi collection: a copy with no selection writes the focused cell as the collection shows it",
          "[covi][ui][table][config]") {
    TableHarness h;
    h.extraViews.insert_or_assign("Lims_ListSamples", std::string{kMixedSamplesView});
    auto const screen = h.mount(kSamplesCollection);
    h.reply(h.dispatcher.last("Lims_ListSamples"), std::string{kFourSamples});
    int const collection = h.byTestId("Table", "collection_rows");
    h.backend.focusMove(collection, FocusMove::First);
    h.backend.focusMove(collection, FocusMove::Down);
    h.backend.focusMove(collection, FocusMove::Right);
    h.settle();
    pressCopy(h, collection);
    CHECK(clipboardText(h) == "Alpha");
    CHECK(statusOf(h, collection).message == "Copied 1 row");
}

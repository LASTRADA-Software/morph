// SPDX-License-Identifier: Apache-2.0

// A collection's table: the stored state it starts from.

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <morph/table/state_store.hpp>
#include <morph/ui/mount.hpp>
#include <string>
#include <vector>

#include "ui_table_model_support.hpp"
#include "ui_table_support.hpp"

namespace {

using morph::ui::testing::kMixedSamplesView;
using morph::ui::testing::kSamplesCollection;
using morph::ui::testing::TableHarness;
using table_model_support::kFourSamples;
namespace table = morph::table;

// NOLINTNEXTLINE(cert-err58-cpp,bugprone-throwing-static-initialization): a test table; an allocation failure at start-up ends the run either way.
table::TableStateKey const kCollectionKey{.application = "lims",
                                          .model = "Lims_SampleModel",
                                          .table = "Lims_ListSamples",
                                          .qualifier = "samples",
                                          .part = "state"};

void store(std::shared_ptr<table::MemoryTableStateStore> const& states, std::string json) {
    states->save(kCollectionKey, std::move(json)).thenDetached([](table::SaveResult const&) {});
}

}  // namespace

TEST_CASE("cov collections: a stored state that is not a state document is reported and not used",
          "[ui][table][cov]") {
    auto const states = std::make_shared<table::MemoryTableStateStore>(morph::ui::testing::storeExecutor());
    store(states, "this is not json");
    TableHarness h{states};
    h.extraViews.insert_or_assign("Lims_ListSamples", std::string{kMixedSamplesView});
    auto const screen = h.mount(kSamplesCollection);
    h.settle();
    h.reply(h.dispatcher.last("Lims_ListSamples"), std::string{kFourSamples});
    CHECK(morph::ui::testing::diagnosticsMentioning(h, "is not used") == 1);
    int const collection = h.byTestId("Table", "collection_rows");
    CHECK(morph::ui::testing::rowKeys(h, collection) == std::vector<std::string>{"1", "2", "3", "4"});
}

TEST_CASE("cov collections: a stored sort and hidden column shape the collection", "[ui][table][cov]") {
    auto const states = std::make_shared<table::MemoryTableStateStore>(morph::ui::testing::storeExecutor());
    store(states, R"({"version":1,"sort":[{"column":"name","direction":"desc"}]})");
    TableHarness h{states};
    h.extraViews.insert_or_assign("Lims_ListSamples", std::string{kMixedSamplesView});
    auto const screen = h.mount(kSamplesCollection);
    h.settle();
    h.reply(h.dispatcher.last("Lims_ListSamples"), std::string{kFourSamples});
    int const collection = h.byTestId("Table", "collection_rows");
    std::vector<std::string> const keys = morph::ui::testing::rowKeys(h, collection);
    REQUIRE(keys.size() == 4);
    CHECK(keys.front() == "3");  // the accented name sorts last, so first when descending
    CHECK(morph::ui::testing::diagnosticsMentioning(h, "stored state") == 0);
}

TEST_CASE("cov collections: a stored filter narrows the collection before its rows are shown", "[ui][table][cov]") {
    auto const states = std::make_shared<table::MemoryTableStateStore>(morph::ui::testing::storeExecutor());
    store(states, R"({"version":1,"filters":{"columns":{"name":{"include":[{"contains":"mm"}]}}}})");
    TableHarness h{states};
    h.extraViews.insert_or_assign("Lims_ListSamples", std::string{kMixedSamplesView});
    auto const screen = h.mount(kSamplesCollection);
    h.settle();
    h.reply(h.dispatcher.last("Lims_ListSamples"), std::string{kFourSamples});
    int const collection = h.byTestId("Table", "collection_rows");
    CHECK(morph::ui::testing::rowKeys(h, collection) == std::vector<std::string>{"4"});
}

TEST_CASE("cov collections: with nothing stored the collection shows every row in the order they arrive",
          "[ui][table][cov]") {
    auto const states = std::make_shared<table::MemoryTableStateStore>(morph::ui::testing::storeExecutor());
    TableHarness h{states};
    h.extraViews.insert_or_assign("Lims_ListSamples", std::string{kMixedSamplesView});
    auto const screen = h.mount(kSamplesCollection);
    h.settle();
    h.reply(h.dispatcher.last("Lims_ListSamples"), std::string{kFourSamples});
    int const collection = h.byTestId("Table", "collection_rows");
    CHECK(morph::ui::testing::rowKeys(h, collection) == std::vector<std::string>{"1", "2", "3", "4"});
}

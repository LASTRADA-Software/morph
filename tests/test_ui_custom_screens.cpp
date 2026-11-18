// SPDX-License-Identifier: Apache-2.0
//
// The custom screens' memory store, and the model through a RemoteServer.

#include <catch2/catch_test_macros.hpp>
#include <morph/core/completion.hpp>
#include <morph/core/executor.hpp>
#include <morph/ui/custom_screens.hpp>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ui_custom_screens_support.hpp"

namespace {

/// The value of a completion the inline executor has already settled.
template <typename T>
T settled(morph::async::Completion<T> completion) {
    std::optional<T> out;
    completion.then([&out](T const& value) { out = value; });
    REQUIRE(out.has_value());
    return std::move(*out);
}

morph::ui::CustomScreen screen(std::string id, std::string person) {
    return morph::ui::CustomScreen{.id = std::move(id),
                                   .title = "Mine",
                                   .basedOn = "lims.base",
                                   .document = "{}",
                                   .savedAt = {},
                                   .savedBy = std::move(person)};
}

}  // namespace

TEST_CASE("MemoryCustomScreenStore keeps each person's screens apart", "[ui][custom-screens][store]") {
    morph::ui::MemoryCustomScreenStore store{morph::exec::detail::inlineExecutor()};
    REQUIRE(settled(store.put({.person = "alice", .app = "lims", .id = "custom:1"}, screen("custom:1", "alice"))));
    REQUIRE(settled(store.put({.person = "bob", .app = "lims", .id = "custom:2"}, screen("custom:2", "bob"))));

    std::vector<morph::ui::CustomScreen> const alices = settled(store.list("alice", "lims"));
    REQUIRE(alices.size() == 1);
    CHECK(alices.front().id == "custom:1");
    CHECK(settled(store.list("alice", "other")).empty());

    CHECK(settled(store.get({.person = "alice", .app = "lims", .id = "custom:1"})).has_value());
    auto const notHers = settled(store.get({.person = "alice", .app = "lims", .id = "custom:2"}));
    REQUIRE_FALSE(notHers);
    CHECK(notHers.error().reason == "no such custom screen");

    CHECK_FALSE(settled(store.remove({.person = "alice", .app = "lims", .id = "custom:2"})));
    CHECK(settled(store.remove({.person = "alice", .app = "lims", .id = "custom:1"})));
    CHECK(settled(store.list("alice", "lims")).empty());

    // Alice's writes under an id Bob also uses leave Bob's screen alone.
    REQUIRE(settled(store.put({.person = "bob", .app = "lims", .id = "custom:2"}, screen("custom:2", "bob"))));
    morph::ui::CustomScreen hers = screen("custom:2", "alice");
    hers.title = "Hers";
    REQUIRE(settled(store.put({.person = "alice", .app = "lims", .id = "custom:2"}, std::move(hers))));
    auto const bobs = settled(store.get({.person = "bob", .app = "lims", .id = "custom:2"}));
    REQUIRE(bobs);
    CHECK(bobs->title == "Mine");
    CHECK(bobs->savedBy == "bob");
    CHECK(settled(store.list("bob", "lims")).size() == 1);

    CHECK(settled(store.remove({.person = "alice", .app = "lims", .id = "custom:2"})));
    CHECK(settled(store.get({.person = "bob", .app = "lims", .id = "custom:2"})).has_value());
    CHECK(settled(store.list("bob", "lims")).size() == 1);
}

TEST_CASE("MemoryCustomScreenStore lists one person's screens ordered by id", "[ui][custom-screens][store]") {
    morph::ui::MemoryCustomScreenStore store{morph::exec::detail::inlineExecutor()};
    REQUIRE(settled(store.put({.person = "alice", .app = "lims", .id = "custom:b"}, screen("custom:b", "alice"))));
    REQUIRE(settled(store.put({.person = "alice", .app = "lims", .id = "custom:a"}, screen("custom:a", "alice"))));
    std::vector<morph::ui::CustomScreen> const listed = settled(store.list("alice", "lims"));
    REQUIRE(listed.size() == 2);
    CHECK(listed[0].id == "custom:a");
    CHECK(listed[1].id == "custom:b");
}

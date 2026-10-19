// SPDX-License-Identifier: Apache-2.0

// The server's watch table on its own: what each connection watches, under which session, and what is kept when a
// session's watches are dropped.

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <morph/core/detail/watch_table.hpp>
#include <morph/core/wire.hpp>
#include <string>
#include <utility>
#include <vector>

namespace {

using morph::backend::detail::Watch;
using morph::backend::detail::WatchTable;
using morph::wire::InstanceTopic;
using morph::wire::WatchBody;

Watch watchAs(std::string const& principal, std::string const& token) {
    Watch watch;
    watch.principal = principal;
    watch.context.principal = principal;
    watch.context.token = token;
    return watch;
}

WatchBody records(std::vector<std::string> names) { return WatchBody{.records = std::move(names), .instances = {}}; }

WatchBody instance(std::string modelType, std::string primary) {
    return WatchBody{.records = {},
                     .instances = {InstanceTopic{.modelType = std::move(modelType), .primary = std::move(primary)}}};
}

}  // namespace

TEST_CASE("watch_of_answers_null_for_an_unknown_connection_or_topic", "[push][watch]") {
    WatchTable table;
    CHECK(table.watchOf(1, "R") == nullptr);
    CHECK(table.watchOf(1, "M", "k") == nullptr);
    REQUIRE(table.add(1, records({"R"}), watchAs("alice", "t")).refusedRecords.empty());
    REQUIRE(table.add(1, instance("M", "k"), watchAs("alice", "t")).refusedInstances.empty());
    CHECK(table.watchOf(1, "Other") == nullptr);
    CHECK(table.watchOf(1, "M", "other") == nullptr);
    CHECK(table.watchOf(2, "R") == nullptr);
    CHECK(table.watchOf(2, "M", "k") == nullptr);
    REQUIRE(table.watchOf(1, "R") != nullptr);
    CHECK(table.watchOf(1, "R")->principal == "alice");
    REQUIRE(table.watchOf(1, "M", "k") != nullptr);
    CHECK(table.watchOf(1, "M", "k")->context.token == "t");
}

// Mutation: drop every watch of the connection, whatever its session — the other session's topics go too.
TEST_CASE("drop_session_keeps_the_other_sessions_watches", "[push][watch]") {
    WatchTable table;
    table.dropSession(1, "alice", "t");
    REQUIRE(table.topicCount(1) == 0);
    static_cast<void>(table.add(1, records({"Mine"}), watchAs("alice", "t")));
    static_cast<void>(table.add(1, instance("M", "mine"), watchAs("alice", "t")));
    static_cast<void>(table.add(1, records({"Theirs"}), watchAs("alice", "other")));
    static_cast<void>(table.add(1, instance("M", "theirs"), watchAs("bob", "t")));
    REQUIRE(table.topicCount(1) == 4);

    table.dropSession(1, "alice", "t");
    CHECK(table.topicCount(1) == 2);
    CHECK(table.watchOf(1, "Mine") == nullptr);
    CHECK(table.watchOf(1, "M", "mine") == nullptr);
    CHECK(table.watchOf(1, "Theirs") != nullptr);
    CHECK(table.watchOf(1, "M", "theirs") != nullptr);

    table.dropSession(1, "alice", "other");
    table.dropSession(1, "bob", "t");
    CHECK(table.topicCount(1) == 0);
    CHECK(table.watchersOf("Theirs").empty());
}

// Mutation: let an instance past the cap — the connection watches more than kMaxWatchTopics topics.
TEST_CASE("an_instance_past_the_cap_is_refused", "[push][watch]") {
    WatchTable table;
    std::vector<std::string> names;
    names.reserve(morph::wire::kMaxWatchTopics);
    for (std::size_t index = 0; index < morph::wire::kMaxWatchTopics; ++index) {
        names.push_back("R" + std::to_string(index));
    }
    REQUIRE(table.add(1, records(names), watchAs("alice", "t")).refusedRecords.empty());
    auto const refused = table.add(1, instance("M", "k"), watchAs("alice", "t"));
    REQUIRE(refused.refusedInstances.size() == 1);
    CHECK(refused.refusedInstances.front().primary == "k");
    CHECK(table.topicCount(1) == morph::wire::kMaxWatchTopics);
    CHECK(table.watchersOf("M", "k").empty());
}

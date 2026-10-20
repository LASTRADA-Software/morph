// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <morph/client/source.hpp>
#include <string>
#include <vector>

#include "probe_app.hpp"

using morph::client::readArgs;

TEST_CASE("readArgs: reads each option in both spellings and leaves the rest", "[client][args]") {
    std::vector<std::string> args{"client", "--server",       "ws://h:1",  "--ui=qt",   "--db=a.db",         "--user",
                                  "ann",    "--screen=s.one", "-platform", "offscreen", "--exit-after-mount"};
    auto const read = readArgs(args);
    REQUIRE(read.has_value());
    CHECK(read->server.value_or("") == "ws://h:1");
    CHECK_FALSE(read->app.has_value());
    CHECK(read->db == "a.db");
    CHECK(read->user == "ann");
    CHECK(read->screen == "s.one");
    CHECK(read->exitAfterMount);
    CHECK(args == std::vector<std::string>{"client", "--ui=qt", "-platform", "offscreen"});
}

TEST_CASE("readArgs: --dev asks for development mode, which is off without it", "[client][args]") {
    std::vector<std::string> args{"client", "--app", "probe", "--dev"};
    auto const read = readArgs(args);
    REQUIRE(read.has_value());
    CHECK(read->dev);
    CHECK_FALSE(read->exitAfterMount);
    CHECK(args == std::vector<std::string>{"client"});

    std::vector<std::string> plain{"client", "--app", "probe"};
    auto const production = readArgs(plain);
    REQUIRE(production.has_value());
    CHECK_FALSE(production->dev);
}

TEST_CASE("readArgs: the last occurrence wins, and nothing after -- is read", "[client][args]") {
    std::vector<std::string> args{"client", "--app", "first", "--app=second", "--", "--server", "ws://h:1"};
    auto const read = readArgs(args);
    REQUIRE(read.has_value());
    CHECK(read->app.value_or("") == "second");
    CHECK_FALSE(read->server.has_value());
    CHECK(args == std::vector<std::string>{"client", "--", "--server", "ws://h:1"});
}

// Mutation: read past the end for a value option given last — this case reads garbage or crashes.
TEST_CASE("readArgs: a value option without its value is refused and the arguments are kept", "[client][args]") {
    std::vector<std::string> args{"client", "--app", "probe", "--db"};
    auto const lastMissing = readArgs(args);
    REQUIRE_FALSE(lastMissing.has_value());
    CHECK(lastMissing.error() == "--db needs a value");
    CHECK(args.size() == 4);

    std::vector<std::string> followed{"client", "--server", "--app", "probe"};
    auto const followedByOption = readArgs(followed);
    REQUIRE_FALSE(followedByOption.has_value());
    CHECK(followedByOption.error() == "--server needs a value");

    std::vector<std::string> empty{"client", "--app="};
    REQUIRE_FALSE(readArgs(empty).has_value());
}

TEST_CASE("readArgs: exactly one of --server and --app", "[client][args]") {
    std::vector<std::string> neither{"client"};
    auto const none = readArgs(neither);
    REQUIRE_FALSE(none.has_value());
    CHECK_THAT(none.error(), Catch::Matchers::ContainsSubstring("exactly one"));
    std::vector<std::string> both{"client", "--server", "ws://h:1", "--app", "probe"};
    REQUIRE_FALSE(readArgs(both).has_value());
}

TEST_CASE("makeSource: an --app that names no linked application is refused, naming the linked ones",
          "[client][args]") {
    morph::client::Args args;
    args.app = "missing";
    auto const made = morph::client::makeSource(args, {probe::app()});
    REQUIRE_FALSE(made.has_value());
    CHECK(made.error() == "no application named 'missing' is linked into this client (linked: probe)");
    args.app = "probe";
    CHECK(morph::client::makeSource(args, {probe::app()}).has_value());
    morph::client::Args remote;
    remote.server = "ws://127.0.0.1:1";
    CHECK(morph::client::makeSource(remote, {}).has_value());
}

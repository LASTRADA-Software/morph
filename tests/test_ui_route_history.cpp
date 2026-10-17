// SPDX-License-Identifier: Apache-2.0

// Route history (spec 3 §10): the route of the screen a shell shows, kept in a history the user moves through with
// back and forward.

#include <catch2/catch_test_macros.hpp>
#include <morph/ui/route_history.hpp>
#include <morph/ui/testing/memory_history.hpp>
#include <optional>
#include <string>
#include <vector>

using morph::ui::fragmentOf;
using morph::ui::RouteHistory;
using morph::ui::routeOfFragment;
using morph::ui::testing::MemoryHistory;

namespace {

using Entries = std::vector<std::string>;

/// A shell of one screen at a time: opening a route shows it, unless it is refused, when the start screen shows.
struct OneScreenShell {
    explicit OneScreenShell(MemoryHistory& memory)
        : history{memory, [this](std::string const& route) { open(route); }} {}

    void open(std::string const& route) {
        opened.push_back(route);
        history.shown(route == "refused" ? std::string{"start"} : route);
    }

    RouteHistory history;
    Entries opened;
};

}  // namespace

// Mutation: accept any fragment's text as a route (the `#x` case reads as `x`).
TEST_CASE("A route is held in a URL fragment as #/<route>", "[ui][route-history]") {
    CHECK(fragmentOf("lims.sample?sampleId=1042") == "#/lims.sample?sampleId=1042");
    CHECK(routeOfFragment(fragmentOf("lims.sample?sampleId=1042")) == "lims.sample?sampleId=1042");
    CHECK(routeOfFragment("#x").empty());
    CHECK(routeOfFragment("").empty());
    CHECK(routeOfFragment("#/").empty());
}

// Mutation: push on the first shown (the start screen adds a step back to the empty page).
TEST_CASE("The first screen replaces the entry, later ones add entries, and an equal route adds none",
          "[ui][route-history]") {
    MemoryHistory memory;
    OneScreenShell shell{memory};
    CHECK(shell.history.initial().empty());
    shell.history.shown("start");
    CHECK(memory.entries() == Entries{"start"});
    shell.history.shown("a");
    shell.history.shown("a");
    shell.history.shown("");  // a screen with no route
    shell.history.shown("b");
    CHECK(memory.entries() == Entries{"start", "a", "b"});
    CHECK(memory.index() == 2);
}

// Mutation: push what a pop shows (forward is lost, and back returns to b).
TEST_CASE("Back and forward open the entry's route without adding entries", "[ui][route-history]") {
    MemoryHistory memory;
    OneScreenShell shell{memory};
    shell.history.shown("start");
    shell.history.shown("a");
    shell.history.shown("b");

    REQUIRE(memory.back());
    CHECK(shell.opened == Entries{"a"});
    REQUIRE(memory.back());
    REQUIRE(memory.forward());
    CHECK(shell.opened == Entries{"a", "start", "a"});
    CHECK(memory.entries() == Entries{"start", "a", "b"});
    CHECK(memory.index() == 1);

    shell.history.shown("c");  // navigating from the middle drops what was forward
    CHECK(memory.entries() == Entries{"start", "a", "c"});
}

// Mutation: leave the refused route in the entry (the history holds `refused`).
TEST_CASE("A refused route the user typed is replaced by the screen shown instead", "[ui][route-history]") {
    MemoryHistory memory;
    OneScreenShell shell{memory};
    shell.history.shown("start");
    memory.follow("refused");
    CHECK(shell.opened == Entries{"refused"});
    CHECK(memory.entries() == Entries{"start", "start"});
}

// Mutation: read the initial route lazily (after the start screen replaced it).
TEST_CASE("The initial route is the one the page was loaded with", "[ui][route-history]") {
    MemoryHistory memory{"b"};
    OneScreenShell shell{memory};
    CHECK(shell.history.initial() == "b");
    shell.history.shown("start");
    CHECK(shell.history.initial() == "b");
    CHECK(memory.entries() == Entries{"start"});
}

// Mutation: keep the callback after destruction (a later back calls into a destroyed history).
TEST_CASE("A route history stops following its history when destroyed", "[ui][route-history]") {
    MemoryHistory memory;
    {
        OneScreenShell shell{memory};
        shell.history.shown("start");
        shell.history.shown("a");
    }
    CHECK(memory.back());  // no callback left to call
    CHECK(memory.current() == "start");
}

// Mutation: restore the flag through `this` after open (a use after free under ASan).
TEST_CASE("An open that destroys its route history is safe", "[ui][route-history]") {
    MemoryHistory memory;
    std::optional<RouteHistory> history;
    history.emplace(memory, [&history](std::string const&) { history.reset(); });
    history->shown("start");
    history->shown("a");
    REQUIRE(memory.back());
    CHECK_FALSE(history.has_value());
}

// Mutation: replace on every shown during a pop (the redirect overwrites the entry it was redirected from).
TEST_CASE("A screen shown after the first in one move back adds an entry, as a redirect does", "[ui][route-history]") {
    MemoryHistory memory;
    std::optional<RouteHistory> history;
    history.emplace(memory, [&history](std::string const& route) {
        history->shown(route);
        history->shown("redirected");
    });
    history->shown("start");
    history->shown("a");
    REQUIRE(memory.back());
    CHECK(memory.entries() == Entries{"start", "redirected"});
    CHECK(memory.index() == 1);
}

// Mutation: move past either end of the history, or report a followed address that is already current.
TEST_CASE("memory history: back and forward stop at the ends, and following the current address reports nothing",
          "[ui][route]") {
    MemoryHistory memory;
    std::vector<std::string> popped;
    memory.onPop([&popped](std::string const& route) { popped.push_back(route); });
    memory.push("a");
    CHECK_FALSE(memory.forward());
    memory.follow(memory.current());
    CHECK(popped.empty());
    while (memory.back()) {
    }
    CHECK_FALSE(memory.back());
    memory.follow("b");
    CHECK(popped.back() == "b");
}

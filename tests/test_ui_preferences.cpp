// SPDX-License-Identifier: Apache-2.0
//
// The preferences model (spec 5 §12b, spec 6 §8 and §12 "Preferences"), through a RemoteServer: every entry is keyed
// by the person the server derived from the verified session, and what is stored is bounded.

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <morph/ui/preferences.hpp>
#include <string>
#include <vector>

#include "ui_preferences_support.hpp"

using prefs_test::Envelope;
using prefs_test::kLoadDensity;
using prefs_test::Server;

TEST_CASE("preferences: a custom store receives the person the server derived, never the one the client claims",
          "[ui][preferences][remote]") {
    Server s;
    Envelope const set = s.call("set",
                                R"({"app":"lims","scope":"lims.results","point":"density","value":"\"compact\"",)"
                                R"("person":"mallory"})",
                                "alice", "mallory");
    REQUIRE(set.kind == "ok");
    CHECK(set.body == R"({"stored":true})");
    Envelope const load = s.call("load", kLoadDensity, "alice", "mallory");
    REQUIRE(load.kind == "ok");
    CHECK(load.body == R"({"values":{"density":"\"compact\""}})");
    CHECK(s.store->calls() == std::vector<std::string>{R"(set alice lims lims.results density "compact")",
                                                       "get alice lims lims.results density"});
}

TEST_CASE("preferences: a request naming another person's key reads and writes nothing of theirs",
          "[ui][preferences][remote]") {
    Server s;
    REQUIRE(s.call("set", R"({"app":"lims","scope":"lims.results","point":"density","value":"\"compact\""})", "bob")
                .kind == "ok");

    // Alice names Bob in the body and in her claimed principal: she reads only her own (absent) entry.
    Envelope const read =
        s.call("load", R"({"app":"lims","scope":"lims.results","points":["density"],"person":"bob"})", "alice", "bob");
    REQUIRE(read.kind == "ok");
    CHECK(read.body == R"({"values":{}})");

    // Her write and her removal land on her own key.
    REQUIRE(s.call("set",
                   R"({"app":"lims","scope":"lims.results","point":"density","value":"\"comfortable\"",)"
                   R"("person":"bob"})",
                   "alice", "bob")
                .kind == "ok");
    Envelope const removed =
        s.call("remove", R"({"app":"lims","scope":"lims.results","point":"density","person":"bob"})", "alice", "bob");
    REQUIRE(removed.kind == "ok");
    CHECK(removed.body == R"({"removed":true})");

    Envelope const bobs = s.call("load", kLoadDensity, "bob");
    REQUIRE(bobs.kind == "ok");
    CHECK(bobs.body == R"({"values":{"density":"\"compact\""}})");
    CHECK(s.store->calls() == std::vector<std::string>{R"(set bob lims lims.results density "compact")",
                                                       "get alice lims lims.results density",
                                                       R"(set alice lims lims.results density "comfortable")",
                                                       "remove alice lims lims.results density",
                                                       "get bob lims lims.results density"});
}

TEST_CASE("preferences: without a verified person nothing is read or written", "[ui][preferences][remote]") {
    Server s;
    REQUIRE(s.call("set", R"({"app":"lims","scope":"lims.results","point":"density","value":"\"compact\""})", "bob")
                .kind == "ok");
    // A claimed principal without a token is cleared by the server: it is nobody.
    Envelope const read = s.call("load", kLoadDensity, "", "bob");
    REQUIRE(read.kind == "ok");
    CHECK(read.body == R"({"values":{}})");
    Envelope const write = s.call(
        "set", R"({"app":"lims","scope":"lims.results","point":"density","value":"\"comfortable\""})", "", "bob");
    REQUIRE(write.kind == "err");
    CHECK(write.message.contains("signed-in person"));
    REQUIRE(s.call("remove", R"({"app":"lims","scope":"lims.results","point":"density"})", "", "bob").kind == "err");
    CHECK(s.store->calls().size() == 1);
}

TEST_CASE("preferences: an oversized value is refused before the store sees it", "[ui][preferences][remote]") {
    Server s{morph::ui::PreferencesOptions{.maxValueBytes = 32}};
    auto const setValue = [&s](std::string const& json) {
        std::string escaped;
        for (char const c : json) {
            if (c == '"') {
                escaped += '\\';
            }
            escaped += c;
        }
        return s.call("set", R"({"app":"lims","scope":"s","point":"p","value":")" + escaped + R"("})", "alice");
    };
    std::string const atBound = "\"" + std::string(30, 'x') + "\"";
    REQUIRE(atBound.size() == 32);
    CHECK(setValue(atBound).kind == "ok");
    Envelope const over = setValue("\"" + std::string(31, 'x') + "\"");
    REQUIRE(over.kind == "err");
    CHECK(over.message.contains("longer than 32 bytes"));
    // Text that is not JSON is refused too, and neither reaches the store.
    CHECK(setValue("{not json").kind == "err");
    CHECK(s.store->calls().size() == 1);
}

TEST_CASE("preferences: a request with a name out of bounds is refused", "[ui][preferences][remote]") {
    Server s;
    CHECK(s.call("set", R"({"scope":"s","point":"p","value":"1"})", "alice").kind == "err");
    CHECK(s.call("load", R"({"app":"lims","scope":"","points":[]})", "alice").kind == "err");
    std::string const longName(morph::ui::kMaxPrefNameBytes + 1, 'p');
    CHECK(s.call("remove", R"({"app":"lims","scope":"s","point":")" + longName + R"("})", "alice").kind == "err");
    std::string points;
    for (std::size_t index = 0; index <= morph::ui::kMaxPrefPoints; ++index) {
        points += (index == 0 ? "\"p" : ",\"p") + std::to_string(index) + "\"";
    }
    CHECK(s.call("load", R"({"app":"lims","scope":"s","points":[)" + points + "]}", "alice").kind == "err");
    CHECK(s.store->calls().empty());
}

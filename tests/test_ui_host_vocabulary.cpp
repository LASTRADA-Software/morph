// SPDX-License-Identifier: Apache-2.0
//
// The host vocabulary (spec 6 §14 and §12 "Host vocabulary"): a registered command runs with checked arguments, an
// unsafe one only over a verified connection, a missing one is skipped and `can` is false for it, and a host value
// is reactive and null when absent. Each case runs through a mounted screen.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <morph/ui/bundle.hpp>
#include <morph/ui/host_vocabulary.hpp>
#include <morph/ui/value.hpp>
#include <stdexcept>
#include <string>
#include <vector>

#include "ui_interpreter_support.hpp"

using morph::ui::HostArg;
using morph::ui::HostCommand;
using morph::ui::HostValue;
using morph::ui::HostVocabulary;
using morph::ui::Value;
using morph::ui::ValueRecord;
using morph::ui::testing::Harness;

namespace {

constexpr char const* kScreen = R"({
  "vocab": "ui/1", "id": "host",
  "view": { "kind": "column", "children": [
    { "kind": "text", "testId": "count", "text": { "fn": "coalesce", "args": [ { "host": "sync.count" }, "-" ] } },
    { "kind": "text", "testId": "absent", "text": { "fn": "coalesce", "args": [ { "host": "nobody" }, "null" ] } },
    { "kind": "text", "testId": "canOpen", "text": { "fn": "format", "args": [ "{0}", { "can": "host:openPath" } ] } },
    { "kind": "text", "testId": "canBeep", "text": { "fn": "format", "args": [ "{0}", { "can": "host:beep" } ] } },
    { "kind": "text", "testId": "canGone", "text": { "fn": "format", "args": [ "{0}", { "can": "host:gone" } ] } },
    { "kind": "button", "testId": "open", "label": "open", "onClick": { "host": "openPath", "args": { "obj": { "path": "/tmp" } } } },
    { "kind": "button", "testId": "openInt", "label": "bad", "onClick": { "host": "openPath", "args": { "obj": { "path": 3 } } } },
    { "kind": "button", "testId": "openNone", "label": "none", "onClick": { "host": "openPath" } },
    { "kind": "button", "testId": "openExtra", "label": "extra",
      "onClick": { "host": "openPath", "args": { "obj": { "path": "/tmp", "mode": "rw" } } } },
    { "kind": "button", "testId": "beep", "label": "beep", "onClick": { "host": "beep" } },
    { "kind": "button", "testId": "gone", "label": "gone", "onClick": { "host": "gone" } } ] } })";

/// A vocabulary with an unsafe `openPath`, a safe `beep` and an `int` value, recording each run.
struct Fixture {
    Fixture() {
        vocabulary.add(HostCommand{.name = "openPath",
                                   .version = 2,
                                   .args = {HostArg{.name = "path", .type = "string"}},
                                   .safeForUntrusted = false,
                                   .run = [this](ValueRecord const& args) {
                                       // Tolerates a wrong type, so a broken argument check fails the `ran` checks
                                       // instead of crashing.
                                       std::string const* const path = args.at("path").asString();
                                       ran.push_back("openPath " +
                                                     (path != nullptr ? *path : std::string{"<not a string>"}));
                                   }});
        vocabulary.add(HostCommand{.name = "beep",
                                   .version = 1,
                                   .args = {HostArg{.name = "times", .type = "int?"}},
                                   .safeForUntrusted = true,
                                   .run = [this](ValueRecord const& args) {
                                       ran.emplace_back(args.at("times").isNull() ? "beep" : "beep n");
                                   }});
        vocabulary.add(
            HostValue{.name = "sync.count", .version = 3, .type = "int", .initial = Value{std::int64_t{2}}});
        vocabulary.connect(h.session);
    }

    void click(std::string const& testId) {
        h.backend.click(h.byTestId("Button", testId));
        h.settle();
    }

    [[nodiscard]] bool reported(std::string const& reason) const {
        return std::ranges::any_of(h.diagnostics,
                                   [&reason](auto const& refusal) { return refusal.message().contains(reason); });
    }

    Harness h;
    HostVocabulary vocabulary;
    std::vector<std::string> ran;
};

}  // namespace

TEST_CASE("host vocabulary: a registered command runs with checked arguments; a mismatch is a reported error",
          "[ui][host]") {
    Fixture f;
    f.vocabulary.setConnectionVerified(true);
    auto const screen = f.h.mount(kScreen);
    f.click("open");
    CHECK(f.ran == std::vector<std::string>{"openPath /tmp"});

    f.click("openInt");
    f.click("openNone");
    f.click("openExtra");
    CHECK(f.ran.size() == 1);
    CHECK(f.reported("argument `path` of host command `openPath` is not a string"));
    CHECK(f.reported("host command `openPath` has no argument `mode`"));

    // An optional argument may be left out, and arrives as null.
    f.click("beep");
    CHECK(f.ran.back() == "beep");
    // Nothing reached the application's InterpreterHost: the vocabulary ran every command.
    CHECK(f.h.host.log.empty());
}

TEST_CASE("host vocabulary: an unsafe command is skipped on an unverified connection", "[ui][host]") {
    Fixture f;
    auto const screen = f.h.mount(kScreen);
    CHECK(f.h.text("canOpen") == "false");
    CHECK(f.h.text("canBeep") == "true");
    f.click("open");
    f.click("beep");
    CHECK(f.ran == std::vector<std::string>{"beep"});
    CHECK(f.reported("host command `openPath` needs a verified connection"));

    f.vocabulary.setConnectionVerified(true);
    f.h.settle();
    CHECK(f.h.text("canOpen") == "true");
    f.click("open");
    CHECK(f.ran.back() == "openPath /tmp");
}

TEST_CASE("host vocabulary: a missing command is skipped and reported, and `can` is false for it", "[ui][host]") {
    Fixture f;
    f.vocabulary.setConnectionVerified(true);
    auto const screen = f.h.mount(kScreen);
    CHECK(f.h.text("canGone") == "false");
    f.click("gone");
    CHECK(f.ran.empty());
    CHECK(f.reported("host command `gone` is missing"));
    // The server's permitted set does not answer for host commands.
    f.h.session.setPermitted({"host:gone"});
    f.h.settle();
    CHECK(f.h.text("canGone") == "false");
    CHECK(f.h.text("canOpen") == "true");
}

TEST_CASE("host vocabulary: a host value is reactive and null when absent", "[ui][host]") {
    Fixture f;
    auto const screen = f.h.mount(kScreen);
    CHECK(f.h.text("count") == "2");
    CHECK(f.h.text("absent") == "null");
    CHECK(f.vocabulary.set("sync.count", Value{std::int64_t{5}}));
    f.h.settle();
    CHECK(f.h.text("count") == "5");
    // A value of the wrong type, or one nobody registered, changes nothing.
    CHECK_FALSE(f.vocabulary.set("sync.count", Value{"many"}));
    CHECK_FALSE(f.vocabulary.set("nobody", Value{std::int64_t{1}}));
    f.h.settle();
    CHECK(f.h.text("count") == "5");
    CHECK(f.h.text("absent") == "null");
}

TEST_CASE("host vocabulary: ui-hello declares every registered name with its version", "[ui][host]") {
    Fixture const f;
    morph::ui::HelloRequest hello;
    f.vocabulary.describe(hello);
    CHECK(hello.hostCommands == std::map<std::string, std::int64_t, std::less<>>{{"beep", 1}, {"openPath", 2}});
    CHECK(hello.hostValues == std::map<std::string, std::int64_t, std::less<>>{{"sync.count", 3}});
}

TEST_CASE("host vocabulary: a registration that cannot be honoured is refused", "[ui][host]") {
    HostVocabulary vocabulary;
    auto const noop = [](ValueRecord const&) {};
    vocabulary.add(HostCommand{.name = "beep", .version = 1, .args = {}, .safeForUntrusted = true, .run = noop});
    CHECK_THROWS_AS(
        vocabulary.add(HostCommand{.name = "beep", .version = 1, .args = {}, .safeForUntrusted = true, .run = noop}),
        std::invalid_argument);
    CHECK_THROWS_AS(
        vocabulary.add(HostCommand{.name = "", .version = 1, .args = {}, .safeForUntrusted = true, .run = noop}),
        std::invalid_argument);
    CHECK_THROWS_AS(
        vocabulary.add(HostCommand{.name = "idle", .version = 1, .args = {}, .safeForUntrusted = true, .run = {}}),
        std::invalid_argument);
    CHECK_THROWS_AS(vocabulary.add(HostCommand{.name = "typo",
                                               .version = 1,
                                               .args = {HostArg{.name = "x", .type = "strng"}},
                                               .safeForUntrusted = true,
                                               .run = noop}),
                    std::invalid_argument);
    CHECK_THROWS_AS(
        vocabulary.add(HostCommand{.name = "twice",
                                   .version = 1,
                                   .args = {HostArg{.name = "x", .type = "int"}, HostArg{.name = "x", .type = "int"}},
                                   .safeForUntrusted = true,
                                   .run = noop}),
        std::invalid_argument);
    CHECK_THROWS_AS(vocabulary.add(HostValue{.name = "n", .version = 1, .type = "int", .initial = Value{"x"}}),
                    std::invalid_argument);
    vocabulary.add(HostValue{.name = "n", .version = 1, .type = "int?", .initial = Value{}});
    CHECK_THROWS_AS(vocabulary.add(HostValue{.name = "n", .version = 1, .type = "int?", .initial = Value{}}),
                    std::invalid_argument);
}

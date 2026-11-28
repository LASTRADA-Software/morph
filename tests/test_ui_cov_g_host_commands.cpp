// SPDX-License-Identifier: Apache-2.0

// A command the application's host cannot carry out, or that has no host to carry it, is reported where the document
// wrote it and ends the command's list without a change.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "ui_table_support.hpp"

namespace {

using morph::ui::InterpreterHost;
using morph::ui::Refusal;
using morph::ui::testing::TableHarness;

struct Unsupported {
    std::string_view name;
    std::string_view command;
    std::string_view reported;
};

std::string screen(std::string_view command) {
    return R"({"vocab":"ui/1","id":"cmds","state":{"s":{"init":0}},"view":{"kind":"column","children":[)"
           R"({"kind":"button","testId":"go","label":"go","onClick":[)" +
           std::string{command} +
           R"(]},{"kind":"text","testId":"s","text":{"ref":"s"}},)"
           R"({"kind":"text","testId":"loc","text":{"env":"locale"}}]}})";
}

}  // namespace

TEST_CASE("covg host commands: what the host cannot do is reported as not supported", "[covg][ui][interpreter]") {
    auto const& c = GENERATE(values<Unsupported>({
        {"a navigation with no answer", R"({"navigate":"lims.other"})", "`navigate` is not supported by the host"},
        {"a navigation asking for an answer", R"({"navigate":"lims.other","onResult":[{"reset":"s"}]})",
         "`navigate` is not supported by the host"},
        {"a screen closing itself", R"({"close":"self"})", "`close: self` is not supported by the host"},
        {"a notice", R"({"notify":"hello"})", "`notify` is not supported by the host"},
    }));
    CAPTURE(c.name);
    for (bool const hosted : {false, true}) {
        CAPTURE(hosted);
        TableHarness h;
        InterpreterHost refusing;
        std::vector<std::string> reports;
        morph::ui::Interpreter interpreter{
            h.session, h.dispatcher,
            morph::ui::InterpreterOptions{
                .host = hosted ? &refusing : nullptr,
                .diagnostics = [&reports](Refusal const& refusal) { reports.push_back(refusal.reason); }}};
        auto mounted = interpreter.mount(
            morph::ui::testing::loadOrFail(screen(c.command), h.session.options().catalog), {}, h.backend, {});
        REQUIRE(mounted.has_value());
        h.settle();
        h.backend.click(h.byTestId("Button", "go"));
        h.settle();
        REQUIRE(reports.size() == 1);
        CHECK(reports.front() == c.reported);
    }
}

TEST_CASE("covg host commands: setting the locale and resetting preferences need no host to carry them",
          "[covg][ui][interpreter]") {
    for (bool const hosted : {false, true}) {
        CAPTURE(hosted);
        TableHarness h;
        InterpreterHost plain;
        std::vector<std::string> reports;
        morph::ui::Interpreter interpreter{
            h.session, h.dispatcher,
            morph::ui::InterpreterOptions{
                .host = hosted ? &plain : nullptr,
                .diagnostics = [&reports](Refusal const& refusal) { reports.push_back(refusal.reason); }}};
        auto mounted =
            interpreter.mount(morph::ui::testing::loadOrFail(
                                  screen(R"({"setLocale":"de"},{"resetPrefs":"all"},{"set":"s","value":{"lit":7}})"),
                                  h.session.options().catalog),
                              {}, h.backend, {});
        REQUIRE(mounted.has_value());
        h.settle();
        h.backend.click(h.byTestId("Button", "go"));
        h.settle();
        CHECK(reports.empty());
        CHECK(h.backend.prop(h.byTestId("Text", "loc"), "text") == "de");
        CHECK(h.backend.prop(h.byTestId("Text", "s"), "text") == "7");
    }
}

TEST_CASE("covg host commands: resetting one preference restores its default and leaves the others",
          "[covg][ui][interpreter]") {
    auto const& which = GENERATE(as<std::string_view>{}, "pick", "all");
    TableHarness h;
    std::string text = screen(std::string{R"({"resetPrefs":")"} + std::string{which} + R"("})");
    text.insert(text.find(R"("state":)"), R"("customize":{"pick":{"kind":"choice","options":["a","b"],"default":"a"},)"
                                          R"("other":{"kind":"choice","options":["x","y"],"default":"x"}},)");
    auto mounted = h.mount(text);
    REQUIRE(mounted->setPreference("pick", morph::ui::Value{std::string{"b"}}));
    REQUIRE(mounted->setPreference("other", morph::ui::Value{std::string{"y"}}));
    h.backend.click(h.byTestId("Button", "go"));
    h.settle();
    CHECK(morph::ui::testing::readJson(*mounted, R"({"pref":"pick"})") == R"("a")");
    CHECK(morph::ui::testing::readJson(*mounted, R"({"pref":"other"})") == (which == "all" ? R"("x")" : R"("y")"));
}

// SPDX-License-Identifier: Apache-2.0

// The names an extra chat tool may have: every character class a provider's format takes is accepted, and every
// other character, or length, is refused when the service is built.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <morph/ui/chat/service.hpp>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

namespace chat = morph::ui::chat;

void build(std::string name) {
    chat::ChatTool tool;
    tool.spec = chat::ToolSpec{.name = std::move(name), .description = "a tool", .inputSchema = {}};
    tool.handler = [](chat::ChatToolContext const&, morph::ui::Json const&, chat::ChatToolDone const&) {};
    chat::ChatOptions options;
    options.tools.push_back(std::move(tool));
    chat::ChatService const service{std::move(options)};
}

}  // namespace

TEST_CASE("covd chat tools: a name of letters, digits, underscores and hyphens is accepted",
          "[chat][service][extra][covd]") {
    auto const name =
        GENERATE(as<std::string>{}, "a", "z", "A", "Z", "0", "9", "tool_1", "tool-1", "_", "-", std::string(64, 'x'));
    CAPTURE(name);
    CHECK_NOTHROW(build(name));
}

TEST_CASE("covd chat tools: a name with any other character, a double underscore or a longer length is refused",
          "[chat][service][extra][covd]") {
    auto const name = GENERATE(as<std::string>{}, "a b", "a.b", "a/b", "a:b", "`", "{", "[", "@", "/", ":", "\xC3\xA9",
                               "a__b", std::string(65, 'x'));
    CAPTURE(name);
    CHECK_THROWS_AS(build(name), std::invalid_argument);
}

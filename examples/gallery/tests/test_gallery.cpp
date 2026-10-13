// SPDX-License-Identifier: Apache-2.0

// First, ahead of every header that brings view.hpp: GCC's -Wshadow reports value.hpp's enumerator `Type::Key` when
// view.hpp's `ui::Key` was declared before it.
// clang-format off
#include <morph/ui/value.hpp>
// clang-format on

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <morph/ui/document.hpp>
#include <set>
#include <string>
#include <string_view>
#include <testkit/screen_harness.hpp>
#include <variant>
#include <vector>

#include "gallery/gallery.hpp"
#include "gallery/screens.hpp"

using morph::examples::testing::RigMode;
using morph::examples::testing::ScreenHarness;

namespace {

/// Every `kind` named in @p json, at any depth: the view's nodes and the components' fallbacks.
void collectKinds(morph::ui::Json const& json, std::set<std::string>& kinds) {
    if (auto const* const object = std::get_if<morph::ui::Json::object_t>(&json.data)) {
        for (auto const& [key, value] : *object) {
            if (auto const* const text = std::get_if<std::string>(&value.data); text != nullptr && key == "kind") {
                kinds.insert(*text);
            }
            collectKinds(value, kinds);
        }
    } else if (auto const* const array = std::get_if<morph::ui::Json::array_t>(&json.data)) {
        for (auto const& item : *array) {
            collectKinds(item, kinds);
        }
    }
}

}  // namespace

TEST_CASE("gallery: the screen registers, validated, and the bundle builds", "[gallery]") {
    auto const built = gallery::bundle();
    INFO((built ? std::string{} : built.error().message()));
    REQUIRE(built.has_value());
    REQUIRE((*built)->screens.size() == 1);
}

// Mutation: drop a node kind from the screen — the missing kind is named here.
TEST_CASE("gallery: uses every node kind of ui/1 but the forms engine's, which wait for it", "[gallery]") {
    auto emitted = gallery::homeScreen().emit();
    REQUIRE(emitted.has_value());
    std::set<std::string> used;
    // The view, and the components' fallbacks; elsewhere `kind` names a customization point's kind.
    auto const& document = std::get<morph::ui::Json::object_t>(emitted->json.data);
    collectKinds(document.at("view"), used);
    collectKinds(document.at("components"), used);
    std::vector<std::string_view> const pending = gallery::pendingKinds();
    std::vector<std::string_view> const kinds = morph::ui::nodeKinds();
    for (std::string_view const kind : kinds) {
        bool const waits = std::ranges::find(pending, kind) != pending.end();
        INFO("kind " << kind);
        CHECK(used.contains(std::string{kind}) != waits);
    }
    for (std::string const& kind : used) {
        INFO("kind " << kind);
        CHECK(std::ranges::find(kinds, std::string_view{kind}) != kinds.end());
    }
}

TEST_CASE("gallery: mounts without a problem, and its query's rows reach the screen", "[gallery]") {
    RigMode const mode = GENERATE(RigMode::Local, RigMode::Simulated);
    INFO("mode " << morph::examples::testing::nameOf(mode));
    ScreenHarness harness{gallery::localApp(), std::string{gallery::kHomeScreen}, mode};
    CHECK(harness.mounted() == std::vector<std::string>{std::string{gallery::kHomeScreen}});
    CHECK(harness.waitForText("count", "3 facts"));
    CHECK(harness.backend().find("Text", "text", "Renderers").has_value());
    CHECK(harness.problems().empty());
    for (std::string const& problem : harness.problems()) {
        UNSCOPED_INFO(problem);
    }
}

TEST_CASE("gallery: a click, an edit and a choice change what the screen shows", "[gallery]") {
    RigMode const mode = GENERATE(RigMode::Local, RigMode::Simulated);
    INFO("mode " << morph::examples::testing::nameOf(mode));
    ScreenHarness harness{gallery::localApp(), std::string{gallery::kHomeScreen}, mode};
    REQUIRE(harness.waitForText("clicks", "0 clicks"));
    harness.click("click");
    CHECK(harness.waitForText("clicks", "1 clicks"));
    harness.edit("name", "Ada");
    CHECK(harness.waitForText("greeting", "Hello, Ada"));
    CHECK(harness.waitForText("flavour-text", "A plain flavour"));
    harness.choose("flavour", "fancy");
    CHECK(harness.waitForText("flavour-text", "A fancy flavour"));
    CHECK(harness.problems().empty());
}

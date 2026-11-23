// SPDX-License-Identifier: Apache-2.0
//
// Runtime tours on the client (docs/spec/ui/tours.md, "The tours of a session"): a listed tour's text loads as a tour,
// `AppRoot::tours()` lists the app document's tours and then the runtime ones, and a running tour that is replaced,
// removed or shortened ends with a toast, while a change of locale does not end it.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <memory>
#include <morph/ui/document.hpp>
#include <morph/ui/interpreter.hpp>
#include <morph/ui/tours.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ui_interpreter_support.hpp"
#include "ui_tours_support.hpp"

namespace ui = morph::ui;

namespace {

constexpr std::string_view kApp = R"({"vocab":"ui/1","id":"app",
  "tours":[{"id":"basics","title":"Basics","steps":[{"title":"One"},{"title":"Two"}]}]})";

/// A runtime tour @p id at @p revision, its title a translated text, with @p steps steps.
ui::TourDecl runtimeTour(std::string const& id, std::uint64_t revision, std::size_t steps = 2,
                         std::string const& stepTitle = "Look") {
    std::string list;
    for (std::size_t index = 0; index < steps; ++index) {
        list += (index == 0 ? "" : ",") + std::string{R"({"title":")"} + (index == 0 ? stepTitle : "More") + R"("})";
    }
    auto loaded = ui::loadTourText(R"({"id":")" + id + R"(","title":{"t":"tours.new","fallback":"What's new"},)" +
                                   R"("steps":[)" + list + "]}");
    REQUIRE(loaded.has_value());
    loaded->revision = revision;
    return std::move(*loaded);
}

struct Rig {
    Rig() {
        h.session.setTranslations([](std::string_view key, std::string_view locale) -> std::optional<std::string> {
            if (key == "tours.new" && locale.starts_with("de")) {
                return std::string{"Neu"};
            }
            return std::nullopt;
        });
        auto mounted = h.interpreter.mountApp(ui::testing::loadOrFail(kApp));
        REQUIRE(mounted.has_value());
        root = std::move(*mounted);
        controller = std::make_unique<ui::TourController>(h.runtime, *root, shell);
    }
    [[nodiscard]] std::vector<std::string> ids() const {
        std::vector<std::string> out;
        for (ui::Tour const& tour : root->tours().get()) {
            out.push_back(tour.id);
        }
        return out;
    }
    ui::testing::Harness h;
    ui::testing::FakeTourShell shell{h.backend};
    std::unique_ptr<ui::AppRoot> root;
    std::unique_ptr<ui::TourController> controller;
};

}  // namespace

// Mutation: make `loadTourText` skip the shape checks (the malformed tour loads).
TEST_CASE("tours runtime: loadTourText reads one tour and refuses a malformed one", "[ui][tours][runtime]") {
    auto const loaded = ui::loadTourText(R"({"id":"a","title":"A","steps":[{"title":"S","target":"x"}]})");
    REQUIRE(loaded.has_value());
    CHECK(loaded->id == "a");
    CHECK(loaded->steps.front().target == "x");
    auto const refused = ui::loadTourText(R"({"id":"a","title":"A","steps":[]})");
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().path == "/steps");
    CHECK_FALSE(ui::loadTourText("not json").has_value());
}

// Mutation: put the runtime tours before the app document's in the computed value (`whats-new` comes first).
TEST_CASE("tours runtime: AppRoot::tours lists the app document's tours, then the runtime ones, texts translated",
          "[ui][tours][runtime]") {
    Rig r;
    CHECK(r.ids() == std::vector<std::string>{"basics"});
    r.root->setRuntimeTours({runtimeTour("whats-new", 1), runtimeTour("basics", 2)});
    r.h.settle();
    // A runtime tour with an app tour's id is left out: the app document's wins.
    CHECK(r.ids() == std::vector<std::string>{"basics", "whats-new"});
    ui::Tour const& added = r.root->tours().get().at(1);
    CHECK(added.revision == 1);
    CHECK(added.title == "What's new");
    CHECK(r.root->tours().get().front().revision == 0);
    r.h.session.setLocale("de");
    r.h.settle();
    CHECK(r.root->tours().get().at(1).title == "Neu");
}

// Mutation: compare only the ids in the controller's follow effect (the replaced tour keeps running).
TEST_CASE("tours runtime: a running tour that is replaced ends with a toast saying it changed",
          "[ui][tours][runtime]") {
    Rig r;
    r.root->setRuntimeTours({runtimeTour("whats-new", 1)});
    r.h.settle();
    REQUIRE(r.controller->start("whats-new").has_value());
    r.root->setRuntimeTours({runtimeTour("whats-new", 2, 2, "Look again")});
    r.h.settle();
    CHECK_FALSE(r.controller->running());
    CHECK_FALSE(r.controller->view().has_value());
    REQUIRE(r.shell.notices.size() == 1);
    CHECK(r.shell.notices.front().first.contains("changed"));
    CHECK(r.shell.notices.front().first.contains("What's new"));
}

// Mutation: skip the missing-tour branch of the follow effect (the removed tour keeps running, view empty).
TEST_CASE("tours runtime: a running tour that is removed ends with a toast", "[ui][tours][runtime]") {
    Rig r;
    r.root->setRuntimeTours({runtimeTour("whats-new", 1)});
    r.h.settle();
    REQUIRE(r.controller->start("whats-new").has_value());
    r.root->setRuntimeTours({});
    r.h.settle();
    CHECK_FALSE(r.controller->running());
    CHECK(r.shell.notices.size() == 1);
    // Nothing indexes the missing steps afterwards.
    r.controller->next();
    r.controller->activeChanged();
    CHECK_FALSE(r.controller->running());
}

// Mutation: drop the `_step >= now->steps.size()` test from the follow effect (a tour that kept its revision but lost
// the step on show keeps running with an empty view).
TEST_CASE("tours runtime: a running tour that lost the step on show ends", "[ui][tours][runtime]") {
    Rig r;
    r.root->setRuntimeTours({runtimeTour("whats-new", 1, 3)});
    r.h.settle();
    REQUIRE(r.controller->start("whats-new").has_value());
    r.controller->next();
    r.controller->next();
    REQUIRE(r.controller->view()->position == 3);
    r.root->setRuntimeTours({runtimeTour("whats-new", 1, 1)});
    r.h.settle();
    CHECK_FALSE(r.controller->running());
    CHECK(r.shell.notices.size() == 1);
}

// Mutation: end on any change of the tours in the follow effect (a locale change ends the tour).
TEST_CASE("tours runtime: a change of locale or of another tour leaves a running tour running",
          "[ui][tours][runtime]") {
    Rig r;
    r.root->setRuntimeTours({runtimeTour("whats-new", 1)});
    r.h.settle();
    REQUIRE(r.controller->start("whats-new").has_value());
    r.h.session.setLocale("de");
    r.h.settle();
    r.root->setRuntimeTours({runtimeTour("whats-new", 1), runtimeTour("other", 2)});
    r.h.settle();
    REQUIRE(r.controller->running());
    CHECK(r.controller->view()->title == "Neu");
    CHECK(r.shell.notices.empty());
}

// Mutation: end whenever the runtime tours change, whatever the running tour (the app tour stops).
TEST_CASE("tours runtime: an app document tour keeps running across a runtime update", "[ui][tours][runtime]") {
    Rig r;
    REQUIRE(r.controller->start("basics").has_value());
    r.root->setRuntimeTours({runtimeTour("whats-new", 1)});
    r.h.settle();
    r.root->setRuntimeTours({runtimeTour("whats-new", 2)});
    r.h.settle();
    r.root->setRuntimeTours({});
    r.h.settle();
    REQUIRE(r.controller->running());
    CHECK(r.controller->view()->stepTitle == "One");
    CHECK(r.shell.notices.empty());
}

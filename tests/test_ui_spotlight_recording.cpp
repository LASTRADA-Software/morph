// SPDX-License-Identifier: Apache-2.0
//
// The recording backend's spotlight (docs/spec/ui/backend_contract.md, "The spotlight"): what it records, how it moves
// the recorded focus, and that the target it rings follows the live widgets without a test setting it by hand.

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <morph/ui/backend.hpp>
#include <morph/ui/testing/recording_backend.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ui = morph::ui;

// Mutation: leave the recorded focus where it is in `FakeSpotlight::setOpen` (the field keeps it on open, and nothing
// hands it back on close).
TEST_CASE("spotlight: opening moves focus to the card's first shown button, and closing hands it back",
          "[ui][tours]") {
    ui::testing::RecordingBackend backend;
    auto const page = backend.createStack(nullptr, ui::Axis::Vertical);
    auto const field = backend.createTextInput(page.get(), ui::TextInputMode::SingleLine);
    auto const spotlight = backend.createSpotlight(nullptr);
    auto const back = backend.createButton(spotlight.get());
    back->setVisible(false);
    auto const next = backend.createButton(spotlight.get());
    field->focus();
    spotlight->setOpen(true);
    CHECK(backend.focused() == backend.idOf(*next));
    spotlight->setOpen(false);
    CHECK(backend.focused() == backend.idOf(*field));
}

// Mutation: resolve the target only in `setTarget` (a later testId, hide or destroy changes nothing recorded).
TEST_CASE("spotlight: the recorded target follows the live widgets", "[ui][tours]") {
    ui::testing::RecordingBackend backend;
    std::vector<bool> heard;
    auto const spotlight = backend.createSpotlight(nullptr);
    int const spotlightId = backend.idOf(*spotlight);
    spotlight->setOnTargetShownChanged([&heard](bool shown) { heard.push_back(shown); });
    spotlight->setTarget(std::string{"later"});
    CHECK(backend.prop(spotlightId, "target") == "later");
    CHECK(backend.prop(spotlightId, "targetShown") == "false");
    auto const page = backend.createStack(nullptr, ui::Axis::Vertical);
    auto target = backend.createButton(page.get());
    target->setTestId("later");
    CHECK(backend.prop(spotlightId, "targetShown") == "true");
    CHECK(backend.prop(spotlightId, "ringed") == std::to_string(backend.idOf(*target)));
    page->setVisible(false);
    CHECK(backend.prop(spotlightId, "targetShown") == "false");
    page->setVisible(true);
    target.reset();
    CHECK(backend.prop(spotlightId, "ringed") == "0");
    CHECK(heard == std::vector<bool>{true, false, true, false});
    spotlight->setTarget(std::nullopt);
    CHECK(backend.prop(spotlightId, "target") == "none");
}

// Mutation: drop `dimmed` from `RecordingBackend::reachable` (a click on the page widget behind the dimmed area runs).
TEST_CASE("spotlight: an open spotlight takes presses only on its card and on the widget it rings", "[ui][tours]") {
    ui::testing::RecordingBackend backend;
    auto const page = backend.createStack(nullptr, ui::Axis::Vertical);
    auto const ringed = backend.createButton(page.get());
    ringed->setTestId("ringed");
    auto const other = backend.createButton(page.get());
    auto const spotlight = backend.createSpotlight(nullptr);
    auto const card = backend.createButton(spotlight.get());
    int ringedClicks = 0;
    int otherClicks = 0;
    int cardClicks = 0;
    ringed->setOnClick([&ringedClicks] { ++ringedClicks; });
    other->setOnClick([&otherClicks] { ++otherClicks; });
    card->setOnClick([&cardClicks] { ++cardClicks; });
    spotlight->setTarget(std::string{"ringed"});
    // Closed, the spotlight dims nothing: the page takes presses, and its card shows nothing.
    backend.click(backend.idOf(*other));
    CHECK(otherClicks == 1);
    spotlight->setOpen(true);
    backend.click(backend.idOf(*other));
    CHECK(otherClicks == 1);
    backend.click(backend.idOf(*ringed));
    CHECK(ringedClicks == 1);
    backend.click(backend.idOf(*card));
    CHECK(cardClicks == 1);
    spotlight->setOpen(false);
    backend.click(backend.idOf(*other));
    CHECK(otherClicks == 2);
}

// Mutation: leave `_notifying` set when a listener throws (the guard removed): the next change tells nobody.
TEST_CASE("spotlight: a listener that throws once does not leave the store deaf", "[ui][tours]") {
    auto const store = std::make_shared<ui::testing::detail::RecordStore>();
    // Declared before the button: its destructor notifies the listener, which reads both.
    int heard = 0;
    bool throwNext = true;
    ui::testing::detail::FakeButton button{store, "Button", nullptr};
    store->listen([&] {
        ++heard;
        if (std::exchange(throwNext, false)) {
            throw std::runtime_error{"listener"};
        }
    });
    CHECK_THROWS_AS(button.setVisible(false), std::runtime_error);
    int const afterThrow = heard;
    button.setVisible(true);
    CHECK(heard == afterThrow + 1);
}

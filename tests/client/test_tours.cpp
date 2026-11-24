// SPDX-License-Identifier: Apache-2.0
//
// The generic client's guided tours (docs/spec/client/client.md, "Tours"), in process: the Tours menu follows the
// active tab, a document's `tour.start` and `startTour` start a tour, a waiting step moves on with the navigator, a
// screen that cannot open ends the tour with a toast, compact rings the navigation menu, and sign-out ends a tour.

// First, ahead of every header that brings view.hpp: GCC's -Wshadow reports value.hpp's enumerator `Type::Key` when
// view.hpp's `ui::Key` was declared before it.
// clang-format off
#include <morph/ui/value.hpp>
// clang-format on

#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <expected>
#include <memory>
#include <morph/client/shell.hpp>
#include <morph/client/source.hpp>
#include <morph/core/backend.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/logger.hpp>
#include <morph/core/wire.hpp>
#include <morph/session/session.hpp>
#include <morph/ui/testing/recording_backend.hpp>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <testkit/fake_app_context.hpp>
#include <testkit/wait.hpp>
#include <utility>
#include <vector>

#include "tours_app.hpp"

namespace client = morph::client;
namespace ui = morph::ui;
using morph::examples::testing::FakeAppContext;
using morph::examples::testing::pumpUntil;

namespace {

using BundleFn = std::expected<std::shared_ptr<ui::ServedBundle const>, ui::Refusal> (*)();

/// The generic client over the tours application, signed in locally as `ann`, until the Tours menu is made.
struct Window {
    explicit Window(BundleFn bundle = &tours::toursBundle, std::string locale = {}, bool waitForMenu = true)
        : source{client::LocalApp{.name = "tours", .bundle = bundle}, client::LocalSetup{.db = {}, .user = "ann"}} {
        std::optional<std::expected<ui::Bundle, ui::ConnectError>> opened;
        source.open(context,
                    [&opened](std::expected<ui::Bundle, ui::ConnectError> result) { opened = std::move(result); });
        REQUIRE(pumpUntil(context.owner(), [&opened] { return opened.has_value(); }));
        if (!opened->has_value()) {
            FAIL(opened->error().message);
        }
        shell = client::mountShell(
            context, backend, std::move(*opened), source,
            client::ShellOptions{.startScreen = "tours.home",
                                 .onProblem = [this](std::string const& text) { problems.push_back(text); },
                                 .locale = std::move(locale)});
        REQUIRE(wait([this] { return find("Menu", "shell_tours").has_value(); }));
        if (waitForMenu) {
            REQUIRE(wait([this] { return !prop("Menu", "shell_tours", "items").empty(); }));
        }
    }
    ~Window() {
        shell.reset();
        source.close();
    }
    Window(Window const&) = delete;
    Window& operator=(Window const&) = delete;
    Window(Window&&) = delete;
    Window& operator=(Window&&) = delete;

    [[nodiscard]] std::optional<int> find(std::string_view kind, std::string const& testId) const {
        return backend.find(kind, "testId", testId);
    }
    [[nodiscard]] std::string prop(std::string_view kind, std::string const& testId, std::string_view name) const {
        std::optional<int> const widget = find(kind, testId);
        return widget ? backend.prop(*widget, name) : std::string{};
    }
    [[nodiscard]] bool shown(std::string_view kind, std::string const& testId) const {
        std::optional<int> const widget = find(kind, testId);
        return widget && backend.prop(*widget, "visible") != "false";
    }
    [[nodiscard]] std::string text(std::string const& testId) const { return prop("Text", testId, "text"); }
    [[nodiscard]] std::string spotlight(std::string_view name) const {
        return prop("Spotlight", "shell_spotlight", name);
    }
    [[nodiscard]] bool reported(std::string_view part) const {
        return std::ranges::any_of(problems, [part](std::string const& line) { return line.contains(part); });
    }
    template <typename Pred>
    bool wait(Pred pred) {
        return pumpUntil(context.owner(), pred);
    }
    void click(std::string const& testId) {
        REQUIRE(wait([this, &testId] { return shown("Button", testId); }));
        backend.click(*find("Button", testId));
    }

    FakeAppContext context;
    ui::testing::RecordingBackend backend;
    client::LocalSource source;
    std::vector<std::string> problems;
    std::unique_ptr<ui::MountedShell> shell;
};

/// Whether @p node is @p ancestor or one of its descendants.
bool descendsFrom(ui::testing::RecordingBackend const& backend, int ancestor, int node) {
    std::vector<int> pending{ancestor};
    while (!pending.empty()) {
        int const current = pending.back();
        pending.pop_back();
        if (current == node) {
            return true;
        }
        auto const children = backend.children(current);
        pending.insert(pending.end(), children.begin(), children.end());
    }
    return false;
}

}  // namespace

// Mutation: list the general tours first in `showToursMenu` (Getting started comes before the screen's tours).
TEST_CASE(
    "Client shell tours: the Tours menu lists the active screen's tours before the general ones and follows the "
    "tab",
    "[client][tours]") {
    Window window;
    std::string items = window.prop("Menu", "shell_tours", "items");
    CHECK_FALSE(items.contains("Register a sample"));
    CHECK(items.contains("General;enabled=false;heading"));
    CHECK(items.contains("Getting started"));
    window.click("nav_tours.samples");
    REQUIRE(
        window.wait([&window] { return window.prop("Menu", "shell_tours", "items").contains("Register a sample"); }));
    items = window.prop("Menu", "shell_tours", "items");
    CHECK(items.find("Samples;enabled=false;heading") < items.find("Register a sample"));
    CHECK(items.find("Register a sample") < items.find("Find a sample"));
    CHECK(items.find("Find a sample") < items.find("General;enabled=false;heading"));
}

// Mutation: skip opening the tour's screen in `TourController::start` (the target is not shown).
TEST_CASE("Client shell tours: a screen's tour started from another screen opens that screen and rings its target",
          "[client][tours]") {
    Window window;
    REQUIRE(window.shell->startTour("register").has_value());
    REQUIRE(window.wait([&window] { return window.spotlight("targetShown") == "true"; }));
    CHECK(window.spotlight("target") == "samples_table");
    REQUIRE(window.wait([&window] { return window.text("tour_title") == "Here"; }));
    CHECK(window.text("tour_eyebrow") == "Register a sample");
}

// Mutation: drop `followTours` from the workspace's change callback (the waiting step never moves on).
TEST_CASE("Client shell tours: a tour from the Tours menu waits for the navigator and moves on when Samples opens",
          "[client][tours]") {
    Window window;
    // On the home screen the menu holds the "General" heading, then Getting started, Locked away and Waits first.
    window.backend.chooseMenuEntry(*window.find("Menu", "shell_tours"), {1});
    REQUIRE(window.wait([&window] { return window.spotlight("open") == "true"; }));
    CHECK(window.spotlight("target") == "shell_tours");
    window.click("tour_next");
    REQUIRE(window.wait([&window] { return window.spotlight("target") == "nav_tours.samples"; }));
    REQUIRE(window.wait([&window] { return !window.shown("Button", "tour_next"); }));
    window.click("nav_tours.samples");
    REQUIRE(window.wait([&window] { return window.spotlight("target") == "samples_table"; }));
}

// Mutation: limit `tour.start` to the card's document (the home screen's button is reported missing).
TEST_CASE("Client shell tours: tour.start from a document starts the tour, and can host:tour.start is true",
          "[client][tours]") {
    Window window;
    REQUIRE(window.wait([&window] { return window.text("can_tour") == "true"; }));
    window.click("take_tour");
    REQUIRE(window.wait([&window] { return window.spotlight("open") == "true"; }));
    REQUIRE(window.wait([&window] { return window.text("tour_eyebrow") == "Getting started"; }));
}

// Mutation: drop the `refusal` of `tour.start` (the unknown id is posted, refused by `start`, and never reported).
TEST_CASE("Client shell tours: tour.start naming a tour the app does not declare is reported", "[client][tours]") {
    Window window;
    window.click("bad_tour");
    REQUIRE(window.wait([&window] { return window.reported("offers no tour `nope`"); }));
    CHECK(window.spotlight("open") != "true");
}

// Mutation: skip the toast in `TourController::fail` (no toast shows).
TEST_CASE("Client shell tours: a step whose screen cannot be opened ends the tour with a toast", "[client][tours]") {
    Window window;
    window.backend.chooseMenuEntry(*window.find("Menu", "shell_tours"), {2});
    REQUIRE(window.wait([&window] {
        std::vector<int> const toasts = window.backend.all("Toast");
        return !toasts.empty() && window.backend.prop(toasts.front(), "text").contains("Closed");
    }));
    CHECK(window.spotlight("open") != "true");
}

// Mutation: show the controller's English text whatever the locale (`TourShell::text` ignores the session's format).
TEST_CASE("Client shell tours: the toast of a tour that stopped is in the person's language", "[client][tours]") {
    Window window{&tours::toursBundle, "de"};
    window.backend.chooseMenuEntry(*window.find("Menu", "shell_tours"), {2});
    REQUIRE(window.wait([&window] {
        std::vector<int> const toasts = window.backend.all("Toast");
        return !toasts.empty() && window.backend.prop(toasts.front(), "text").contains("Die Tour wurde beendet");
    }));
    CHECK(window.backend.prop(window.backend.all("Toast").front(), "text").contains("konnte nicht geöffnet werden"));
}

// Mutation: ignore the frame's form in the shell's `TourShell::compact` (the navigator entry is rung as itself).
TEST_CASE("Client shell tours: compact, the Tours entries join the navigation menu and a navigator target is the menu",
          "[client][tours]") {
    Window window;
    window.backend.setWidthClass("medium");
    REQUIRE(window.wait([&window] { return window.prop("Menu", "shell_nav_menu", "items").contains("Tours"); }));
    CHECK(window.prop("Menu", "shell_tours", "visible") == "false");
    REQUIRE(window.shell->startTour("getting-started").has_value());
    window.click("tour_next");
    REQUIRE(window.wait([&window] { return window.spotlight("target") == "shell_nav_menu"; }));
}

// Mutation: drop the `formChanged` call from `followWidthClass` (the ring stays where the old form put it).
TEST_CASE("Client shell tours: a change of width mid-step moves the ring between the navigator entry and the menu",
          "[client][tours]") {
    Window window;
    REQUIRE(window.shell->startTour("getting-started").has_value());
    REQUIRE(window.wait([&window] { return window.spotlight("open") == "true"; }));
    window.click("tour_next");
    REQUIRE(window.wait([&window] { return window.spotlight("target") == "nav_tours.samples"; }));
    window.backend.setWidthClass("medium");
    REQUIRE(window.wait([&window] { return window.spotlight("target") == "shell_nav_menu"; }));
    window.backend.setWidthClass("expanded");
    REQUIRE(window.wait([&window] { return window.spotlight("target") == "nav_tours.samples"; }));
}

// Mutation: make the frame's spotlight a backend root (`createSpotlight(nullptr)`), so it outlives the frame.
TEST_CASE("Client shell tours: signing out ends a running tour", "[client][tours]") {
    Window window;
    // A tour step opens a screen that signs the person out as it mounts: the sign-out arrives from a document's
    // `signOut` command while the spotlight is open, not from a press through the scrim.
    REQUIRE(window.shell->startTour("expires").has_value());
    REQUIRE(window.wait([&window] { return window.spotlight("open") == "true"; }));
    window.click("tour_next");
    REQUIRE(window.wait([&window] {
        std::vector<int> const spotlights = window.backend.all("Spotlight");
        return std::ranges::none_of(spotlights,
                                    [&window](int id) { return window.backend.prop(id, "open") == "true"; });
    }));
}

// Mutation: answer every id from `startTour` with success (the unknown id is not refused).
TEST_CASE("Client shell tours: startTour refuses a tour the app does not declare", "[client][tours]") {
    Window window;
    auto const refused = window.shell->startTour("nope");
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().contains("`nope`"));
}

// Mutation: show the Tours button whatever the active screen offers (it shows on the home screen of an app whose tours
// all belong to Samples), or never show it (it stays hidden on Samples).
TEST_CASE(
    "Client shell tours: the Tours button shows only when the active screen has tours or the app has general ones",
    "[client][tours]") {
    Window window{&tours::screenToursBundle, {}, false};
    REQUIRE(window.wait([&window] { return window.text("home_text") == "Home"; }));
    CHECK_FALSE(window.shown("Menu", "shell_tours"));
    window.click("nav_tours.samples");
    REQUIRE(window.wait([&window] { return window.shown("Menu", "shell_tours"); }));
    window.click("nav_tours.home");
    REQUIRE(window.wait([&window] { return !window.shown("Menu", "shell_tours"); }));
}

// Mutation: show the Tours button whatever the application declares (it shows with no tour at all).
TEST_CASE("Client shell tours: an application with no tours shows no Tours button", "[client][tours]") {
    Window window{&tours::noToursBundle, {}, false};
    REQUIRE(window.wait([&window] { return window.text("home_text") == "Home"; }));
    CHECK_FALSE(window.shown("Menu", "shell_tours"));
}

// Mutation: read the caption, the tooltip or the heading from the English text whatever the locale (the German
// shell shows "Tours", "Guided tours" or "General").
TEST_CASE("Client shell tours: the frame's texts reach the button's caption, its tooltip and the General heading",
          "[client][tours]") {
    {
        Window const window;
        CHECK(window.prop("Menu", "shell_tours", "label") == "Tours");
        CHECK(window.prop("Menu", "shell_tours", "tooltip") == "Guided tours");
        CHECK(window.prop("Menu", "shell_tours", "items").contains("General;enabled=false;heading"));
    }
    Window const german{&tours::toursBundle, "de"};
    CHECK(german.prop("Menu", "shell_tours", "label") == "Touren");
    CHECK(german.prop("Menu", "shell_tours", "tooltip") == "Geführte Touren");
    CHECK(german.prop("Menu", "shell_tours", "items").contains("Allgemein;enabled=false;heading"));
}

namespace {

/// The tours application over an in-process service, whose answers the test can stop: the reload after a sign-out then
/// fails.
class FlakySource final : public client::Source {
public:
    FlakySource() = default;
    ~FlakySource() override { close(); }
    FlakySource(FlakySource const&) = delete;
    FlakySource& operator=(FlakySource const&) = delete;
    FlakySource(FlakySource&&) = delete;
    FlakySource& operator=(FlakySource&&) = delete;

    /// Makes every later exchange fail.
    void breakExchange() { _broken = true; }

protected:
    [[nodiscard]] std::expected<Transport, ui::ConnectError> connect(ui::AppContext& /*context*/) override {
        auto served = tours::toursBundle();
        REQUIRE(served.has_value());
        _service = std::make_shared<ui::UiService>(std::move(*served), morph::model::detail::defaultDispatcher());
        _workers = std::make_unique<morph::exec::ThreadPoolExecutor>(1);
        return Transport{
            .backend = std::make_unique<morph::backend::LocalBackend>(*_workers),
            .exchange = [this](morph::wire::Envelope const& request,
                               std::function<void(ui::ExchangeReply)> const& done) {
                if (_broken) {
                    done(std::unexpected(std::string{"the test broke the exchange"}));
                    return;
                }
                done(morph::wire::decode(_service->handle(request, *morph::session::allowAllAuthorizer())));
            }};
    }
    void disconnect() override {
        _workers.reset();
        _service.reset();
    }

private:
    std::atomic<bool> _broken{false};
    std::shared_ptr<ui::UiService> _service;
    std::unique_ptr<morph::exec::ThreadPoolExecutor> _workers;
};

}  // namespace

/// The ids of the screen's `refresh` head action, after the shell has had time to mount every head it makes.
std::vector<int> headActions(Window& window) {
    window.wait(
        [&window] { return !window.backend.all("Button").empty() && window.find("Button", "head_action_refresh"); });
    window.context.owner().runFor(std::chrono::milliseconds{50});
    std::vector<int> actions;
    for (int const id : window.backend.all("Button")) {
        if (window.backend.prop(id, "testId") == "head_action_refresh") {
            actions.push_back(id);
        }
    }
    return actions;
}

/// Whether @p widget sits in the shell's own head slot.
bool inHeadSlot(Window& window, int widget) {
    std::optional<int> const slot = window.find("Row", "shell_head");
    return slot && descendsFrom(window.backend, *slot, widget);
}

// Control for the case below: with no tour declared, the shell mounts the head itself, in its slot (so the slot is
// where a head action goes when the page does not hold it).
TEST_CASE("Client shell tours: an application with no tours has the head action in the shell's head slot",
          "[client][tours]") {
    Window window{&tours::noToursBundle, {}, false};
    std::vector<int> const actions = headActions(window);
    REQUIRE(actions.size() == 1);
    CHECK(inHeadSlot(window, actions.front()));
}

// Mutation: leave the tours out of `_headInPage` (the head action moves from the page into the shell's head slot).
TEST_CASE("Client shell tours: a bundle that declares only tours keeps the screens' heads in the page",
          "[client][tours]") {
    Window window;
    std::vector<int> const actions = headActions(window);
    REQUIRE(actions.size() == 1);
    CHECK_FALSE(inHeadSlot(window, actions.front()));
}

// Mutation: do not move the focus into the spotlight when it opens (the focus stays on the screen's own button), or
// do not hand it back when the tour ends.
TEST_CASE("Client shell tours: opening the spotlight focuses Next, and ending the tour returns the focus",
          "[client][tours]") {
    Window window;
    int const origin = *window.find("Button", "take_tour");
    window.backend.widget(origin)->focus();
    REQUIRE(window.backend.focused() == origin);
    REQUIRE(window.shell->startTour("getting-started").has_value());
    REQUIRE(window.wait([&window] { return window.spotlight("open") == "true"; }));
    CHECK(window.backend.prop(window.backend.focused(), "testId") == "tour_next");
    window.click("tour_end");
    REQUIRE(window.wait([&window] { return window.spotlight("open") == "false"; }));
    CHECK(window.backend.focused() == origin);
}

// Mutation: as above, but for a tour whose first step waits: Next is hidden, so the focus goes to End tour.
TEST_CASE("Client shell tours: opening the spotlight on a waiting step focuses End tour", "[client][tours]") {
    Window window;
    int const origin = *window.find("Button", "take_tour");
    window.backend.widget(origin)->focus();
    REQUIRE(window.shell->startTour("waits").has_value());
    REQUIRE(window.wait([&window] { return window.spotlight("open") == "true"; }));
    CHECK(window.backend.prop(window.backend.focused(), "testId") == "tour_end");
    window.click("tour_end");
    REQUIRE(window.wait([&window] { return window.spotlight("open") == "false"; }));
    CHECK(window.backend.focused() == origin);
}

// Mutation: drop `_tours->end()` from `resetTours` (the controller goes but the spotlight stays open over an emptied
// card), drop `resetTours` from `finishSignOut` (the controller keeps running with no screens), or drop the frame's
// `setTours({})` after it (the Tours button stays visible).
TEST_CASE("Client shell tours: a sign-out whose reload fails closes the spotlight and leaves no focus in it",
          "[client][tours]") {
    FakeAppContext context;
    ui::testing::RecordingBackend backend;
    FlakySource source;
    std::vector<std::string> problems;
    std::optional<std::expected<ui::Bundle, ui::ConnectError>> opened;
    source.open(context,
                [&opened](std::expected<ui::Bundle, ui::ConnectError> result) { opened = std::move(result); });
    REQUIRE(pumpUntil(context.owner(), [&opened] { return opened.has_value(); }));
    REQUIRE(opened->has_value());
    {
        auto const shell = client::mountShell(
            context, backend, std::move(*opened), source,
            client::ShellOptions{.startScreen = "tours.home",
                                 .onProblem = [&problems](std::string const& text) { problems.push_back(text); }});
        auto const find = [&backend](std::string_view kind, std::string_view testId) {
            return backend.find(kind, "testId", std::string{testId});
        };
        REQUIRE(pumpUntil(context.owner(), [&find] { return find("Button", "take_tour").has_value(); }));
        backend.widget(*find("Button", "take_tour"))->focus();
        REQUIRE(shell->startTour("expires").has_value());
        REQUIRE(pumpUntil(context.owner(), [&] {
            return find("Spotlight", "shell_spotlight") &&
                   backend.prop(*find("Spotlight", "shell_spotlight"), "open") == "true";
        }));
        int const spotlight = *find("Spotlight", "shell_spotlight");
        source.breakExchange();
        // The next step opens the screen that signs out as it mounts; the card's own button is not behind the scrim.
        REQUIRE(pumpUntil(context.owner(), [&find] { return find("Button", "tour_next").has_value(); }));
        backend.click(*find("Button", "tour_next"));
        REQUIRE(pumpUntil(context.owner(), [&problems] {
            return std::ranges::any_of(problems,
                                       [](std::string const& line) { return line.contains("could not be loaded"); });
        }));
        REQUIRE(find("Spotlight", "shell_spotlight") == spotlight);
        CHECK(backend.prop(spotlight, "open") == "false");
        CHECK_FALSE(descendsFrom(backend, spotlight, backend.focused()));
        // The frame stays, and its Tours button goes: its choices would start nothing.
        REQUIRE(find("Menu", "shell_tours").has_value());
        CHECK(backend.prop(*find("Menu", "shell_tours"), "visible") == "false");
    }
    source.close();
}

// Mutation: drop the `_tourSync->list()` call from `buildFrame` (the runtime tour never reaches the menu).
TEST_CASE("Client shell tours: a runtime tour registered on the server is offered after the app document's",
          "[client][tours][runtime]") {
    std::shared_ptr<ui::TourRegistry> const registry = tours::toursRegistry();
    REQUIRE(registry->add(tours::runtimeTour("whats-new")).has_value());
    Window window;
    REQUIRE(window.wait([&window] { return window.prop("Menu", "shell_tours", "items").contains("What's new"); }));
    std::string const items = window.prop("Menu", "shell_tours", "items");
    CHECK(items.find("Locked away") < items.find("What's new"));
    REQUIRE(window.shell->startTour("whats-new").has_value());
    REQUIRE(window.wait([&window] { return window.text("tour_title") == "News"; }));
}

// The control: a server that never registered `morph.tours` offers the app document's tours and nothing else. The sync
// logs the absent model once, which is how the test knows the failed list has answered before it reads the menu.
// Mutation: make a failed list drop the app's tours, or add the registry's tour to the menu without the model.
TEST_CASE("Client shell tours: without the runtime tours model the app document's tours are offered alone",
          "[client][tours][runtime]") {
    std::mutex logMutex;
    std::vector<std::string> logged;
    morph::log::ScopedLoggerOverride const guard{[&logMutex, &logged](morph::log::LogLevel, std::string_view message) {
                                                     std::scoped_lock const lock{logMutex};
                                                     logged.emplace_back(message);
                                                 },
                                                 morph::log::LogLevel::debug};
    Window window;
    REQUIRE(window.wait([&logMutex, &logged] {
        std::scoped_lock const lock{logMutex};
        return std::ranges::any_of(
            logged, [](std::string const& line) { return line.contains("runtime tours: the server offers none"); });
    }));
    std::string const items = window.prop("Menu", "shell_tours", "items");
    CHECK(items.contains("Getting started"));
    CHECK_FALSE(items.contains("What's new"));
    CHECK_FALSE(window.reported("morph.tours"));
}

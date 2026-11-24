// SPDX-License-Identifier: Apache-2.0
//
// The edge paths of guided tours (docs/spec/ui/tours.md): the runtime client's failures and malformed replies, the
// server registry's refusals and its texts, the bundle checks of shell parts, `nav_` ids and `tour.start` arguments,
// the controller's idle and stale calls, and the card binding without a frame, without a post, and after it is gone.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <glaze/glaze.hpp>
#include <map>
#include <memory>
#include <morph/core/bridge.hpp>
#include <morph/core/logger.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <morph/core/wire.hpp>
#include <morph/session/session.hpp>
#include <morph/testing/step_executor.hpp>
#include <morph/ui/author.hpp>
#include <morph/ui/backend.hpp>
#include <morph/ui/host.hpp>
#include <morph/ui/host_vocabulary.hpp>
#include <morph/ui/interpreter.hpp>
#include <morph/ui/service.hpp>
#include <morph/ui/shell_parts.hpp>
#include <morph/ui/shell_parts_model.hpp>
#include <morph/ui/testing/recording_backend.hpp>
#include <morph/ui/tour_card.hpp>
#include <morph/ui/tour_registry.hpp>
#include <morph/ui/tour_sync.hpp>
#include <morph/ui/tours.hpp>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "test_support.hpp"
#include "ui_interpreter_support.hpp"
#include "ui_service_support.hpp"

namespace ui = morph::ui;
using morph::log::LogLevel;

namespace {

/// What the logger was given while a test ran.
class LogCapture {
public:
    LogCapture()
        : _override{[this](LogLevel severity, std::string_view message) {
              _lines.emplace_back(severity, std::string{message});
          }} {}

    /// Whether a line of @p severity contains @p part.
    [[nodiscard]] bool saw(LogLevel severity, std::string_view part) const {
        return std::ranges::any_of(
            _lines, [&](auto const& line) { return line.first == severity && line.second.contains(part); });
    }

    /// How many lines of @p severity there are.
    [[nodiscard]] std::size_t count(LogLevel severity) const {
        return static_cast<std::size_t>(
            std::ranges::count_if(_lines, [&](auto const& line) { return line.first == severity; }));
    }

private:
    std::vector<std::pair<LogLevel, std::string>> _lines;
    morph::log::ScopedLoggerOverride _override;
};

// ---------------------------------------------------------------------------------------------------------------
// The runtime client.
// ---------------------------------------------------------------------------------------------------------------

std::string goodTour(std::string const& id) {
    return R"({"id":")" + id + R"(","title":"Good","steps":[{"title":"Here"}]})";
}

/// The reply of `list` holding @p tours.
std::string listing(std::vector<ui::ListedTour> tours) {
    ui::ToursListed listed;
    listed.tours = std::move(tours);
    return glz::write_json(listed).value_or("{}");
}

/// A sync over a dispatcher whose replies the test sends, and no session.
struct SyncRig {
    void pump() { owner.runAll(); }
    [[nodiscard]] std::size_t asked() const { return dispatcher.count(ui::kToursList); }
    /// Answers the last call with @p json and delivers it.
    void answer(std::string json) {
        dispatcher.reply(dispatcher.last(ui::kToursList), std::move(json));
        pump();
    }
    /// Fails the last call with @p text and delivers that.
    void fail(std::string const& text) {
        dispatcher.fail(dispatcher.last(ui::kToursList), "error", text);
        pump();
    }
    /// Offers the model: a list answered with no tours.
    void offer() {
        sync.list();
        answer(listing({}));
    }

    morph::testing::StepExecutor owner;
    ui::testing::ScriptedDispatcher dispatcher{owner};
    ui::TourSync sync{dispatcher};
};

}  // namespace

TEST_CASE("tours edges: a tour this client cannot load is left out with a warning, the others are kept",
          "[ui][tours][sync]") {
    LogCapture const log;
    SyncRig r;
    r.sync.list();
    REQUIRE(r.asked() == 1);
    r.answer(listing({ui::ListedTour{.id = "good", .revision = 3, .tour = goodTour("good")},
                      ui::ListedTour{.id = "broken", .revision = 4, .tour = R"({"id":"Bad Id","title":"x"})"}}));
    CHECK(r.sync.offered());
    REQUIRE(r.sync.tours().size() == 1);
    CHECK(r.sync.tours().front().id == "good");
    CHECK(r.sync.tours().front().revision == 3);
    CHECK(log.saw(LogLevel::warn, "`broken` does not load in this client"));
}

TEST_CASE("tours edges: a list asked again after the model is offered replaces the list and offers once",
          "[ui][tours][sync]") {
    SyncRig r;
    std::vector<std::size_t> sizes;
    r.sync.setOnListed([&sizes](std::vector<ui::TourDecl> const& tours) { sizes.push_back(tours.size()); });
    r.sync.list();
    r.answer(listing({ui::ListedTour{.id = "one", .revision = 1, .tour = goodTour("one")}}));
    std::optional<bool> again;
    r.sync.list([&again](bool offered) { again = offered; });
    REQUIRE(r.asked() == 2);
    r.answer(listing({ui::ListedTour{.id = "one", .revision = 1, .tour = goodTour("one")},
                      ui::ListedTour{.id = "two", .revision = 2, .tour = goodTour("two")}}));
    REQUIRE(again.has_value());
    CHECK(*again);
    CHECK(r.sync.tours().size() == 2);
    CHECK(sizes == std::vector<std::size_t>{1, 2});
}

TEST_CASE("tours edges: a sync without a session or callbacks lists, relists and resets", "[ui][tours][sync]") {
    SyncRig r;
    // A reset before the model is offered has nothing to unwatch.
    r.sync.reset();
    CHECK_FALSE(r.sync.offered());
    r.offer();
    CHECK(r.sync.offered());
    r.sync.relist();
    REQUIRE(r.asked() == 2);
    r.answer(listing({ui::ListedTour{.id = "fresh", .revision = 1, .tour = goodTour("fresh")}}));
    REQUIRE(r.sync.tours().size() == 1);
    r.sync.reset();
    CHECK_FALSE(r.sync.offered());
    CHECK(r.sync.tours().empty());
}

TEST_CASE("tours edges: a list that fails for another reason than an absent model warns and is not offered",
          "[ui][tours][sync]") {
    LogCapture const log;
    SyncRig r;
    std::optional<bool> offered;
    r.sync.list([&offered](bool answer) { offered = answer; });
    r.fail("the link is down");
    REQUIRE(offered.has_value());
    CHECK_FALSE(*offered);
    CHECK_FALSE(r.sync.offered());
    CHECK(log.saw(LogLevel::warn, "the link is down"));
    // Without a callback the failure is only logged.
    r.sync.list();
    r.fail("still down");
    CHECK(log.saw(LogLevel::warn, "still down"));
    CHECK_FALSE(r.sync.offered());
}

TEST_CASE("tours edges: a reconnect asks again unless the server is known to have no runtime tours",
          "[ui][tours][sync]") {
    SyncRig r;
    r.sync.list();
    r.fail("the link is down");
    r.sync.reconnected();
    REQUIRE(r.asked() == 2);
    r.dispatcher.failWithMessage(r.dispatcher.last(ui::kToursList),
                                 "unknown model type: " + std::string{ui::kToursModel});
    r.pump();
    r.sync.reconnected();
    CHECK(r.asked() == 2);
    CHECK_FALSE(r.sync.offered());
}

TEST_CASE("tours edges: a relist before the model is offered answers at once and asks nothing", "[ui][tours][sync]") {
    SyncRig r;
    bool done = false;
    r.sync.relist([&done] { done = true; });
    CHECK(done);
    r.sync.relist();
    CHECK(r.asked() == 0);
    CHECK_FALSE(r.sync.offered());
}

TEST_CASE("tours edges: a relist that fails keeps the last list, warns and still answers", "[ui][tours][sync]") {
    LogCapture const log;
    SyncRig r;
    r.sync.list();
    r.answer(listing({ui::ListedTour{.id = "kept", .revision = 1, .tour = goodTour("kept")}}));
    bool done = false;
    r.sync.relist([&done] { done = true; });
    r.fail("the link dropped");
    CHECK(done);
    CHECK(log.saw(LogLevel::warn, "the link dropped"));
    REQUIRE(r.sync.tours().size() == 1);
    CHECK(r.sync.tours().front().id == "kept");
    r.sync.relist();
    r.fail("dropped again");
    CHECK(log.saw(LogLevel::warn, "dropped again"));
    CHECK(r.sync.tours().size() == 1);
}

TEST_CASE("tours edges: events are ignored before the model is offered, off topic and when malformed",
          "[ui][tours][sync]") {
    SyncRig r;
    morph::wire::EventBody const resync{.topic = std::string{morph::wire::kResyncTopic},
                                        .payload = glz::raw_json{"null"}};
    r.sync.deliverEvent(resync);
    CHECK(r.asked() == 0);
    r.offer();
    REQUIRE(r.asked() == 1);
    r.sync.deliverEvent(morph::wire::EventBody{.topic = "some.other.topic", .payload = glz::raw_json{"null"}});
    r.sync.deliverEvent(morph::wire::EventBody{.topic = std::string{morph::wire::kChangesTopic},
                                               .payload = glz::raw_json{R"("not a changes body")"}});
    CHECK(r.asked() == 1);
    r.sync.deliverEvent(resync);
    CHECK(r.asked() == 2);
}

// ---------------------------------------------------------------------------------------------------------------
// The server's registry.
// ---------------------------------------------------------------------------------------------------------------

namespace {

std::string resultsTour(std::string const& id, std::string const& title = R"("Results")") {
    return R"({"id":")" + id + R"(","screen":"lims.results","title":)" + title + R"(,"steps":[{"title":"Here"}]})";
}

/// The service fixture's registry with message catalogues for @p locales, each with the key `tours.new.title`.
ui::UiRegistry catalogued(std::vector<std::string> const& locales) {
    ui::UiRegistry registry = uiservice::registry();
    for (std::string const& locale : locales) {
        auto const added =
            registry.addFile(uiservice::file("messages", "i18n/" + locale + ".json", R"({"tours.new.title":"New"})"));
        REQUIRE(added.has_value());
    }
    return registry;
}

std::shared_ptr<ui::ServedBundle const> built(ui::UiRegistry const& registry) {
    auto result = registry.build();
    REQUIRE(result.has_value());
    return *result;
}

struct Held {
    explicit Held(std::shared_ptr<ui::ServedBundle const> served) : bundle{std::move(served)} {}
    std::shared_ptr<ui::ServedBundle const> bundle;
    std::shared_ptr<ui::TourRegistry> tours =
        std::make_shared<ui::TourRegistry>([this] { return std::shared_ptr<ui::ServedBundle const>{bundle}; });
};

/// A text title whose key is @p key.
std::string textTitle(std::string_view key) { return R"({"t":")" + std::string{key} + R"(","fallback":"Fallback"})"; }

}  // namespace

TEST_CASE("tours edges: a registry needs a bundle getter, and registerTours a registry and an authorizer",
          "[ui][tours][runtime]") {
    CHECK_THROWS_AS(ui::TourRegistry{std::function<std::shared_ptr<ui::ServedBundle const>()>{}},
                    std::invalid_argument);
    Held const held{uiservice::bundle()};
    morph::model::detail::ActionDispatcher dispatcher;
    morph::model::detail::ModelRegistryFactory registry;
    auto const authorizer = std::make_shared<uiservice::RuleAuthorizer>();
    CHECK_THROWS_AS(ui::registerTours(nullptr, authorizer, dispatcher, registry), std::invalid_argument);
    CHECK_THROWS_AS(ui::registerTours(held.tours, nullptr, dispatcher, registry), std::invalid_argument);
}

TEST_CASE("tours edges: a getter that answers no bundle is a logic error when a tour is added or rechecked",
          "[ui][tours][runtime]") {
    ui::TourRegistry empty{[] { return std::shared_ptr<ui::ServedBundle const>{}; }};
    CHECK_THROWS_AS(static_cast<void>(empty.add(resultsTour("a"))), std::logic_error);
    CHECK_THROWS_AS(empty.recheck(), std::logic_error);
}

TEST_CASE("tours edges: a submission that is not a tour object is refused with no tour id", "[ui][tours][runtime]") {
    Held const held{uiservice::bundle()};
    for (std::string_view const text : {"not json", "[1,2]", "7"}) {
        auto const refused = held.tours->add(text);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().tourId.empty());
        CHECK(refused.error().path.empty());
        CHECK(refused.error().reason.contains("JSON object"));
        CHECK(refused.error().message().starts_with("the tour `` at /: "));
    }
    // An object whose id is missing or not text has no id to name either.
    auto const missing = held.tours->add(R"({"title":"T","steps":[{"title":"S"}]})");
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().tourId.empty());
    auto const numeric = held.tours->add(R"({"id":5,"title":"T","steps":[{"title":"S"}]})");
    REQUIRE_FALSE(numeric.has_value());
    CHECK(numeric.error().tourId.empty());
}

TEST_CASE("tours edges: a bundle without an app document refuses every tour", "[ui][tours][runtime]") {
    ui::UiRegistry made{ui::ApplicationInfo{.id = "lims", .version = "1"}, uiservice::facts()};
    REQUIRE(made.addScreen(uiservice::entry("lims.results"), std::string{uiservice::kResults}).has_value());
    Held const held{built(made)};
    auto const refused = held.tours->add(resultsTour("a"));
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().tourId == "a");
    CHECK(refused.error().reason.contains("no app document"));
}

TEST_CASE("tours edges: replace refuses an id no runtime tour has, and tells nobody", "[ui][tours][runtime]") {
    Held const held{uiservice::bundle()};
    int changes = 0;
    held.tours->setOnChanged([&changes] { ++changes; });
    auto const refused = held.tours->replace(resultsTour("never-added"));
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().tourId == "never-added");
    CHECK(refused.error().path == "/id");
    CHECK(refused.error().reason.contains("no runtime tour has this id"));
    CHECK(changes == 0);
}

TEST_CASE("tours edges: a text that is not a message reference needs no catalogue key", "[ui][tours][runtime]") {
    Held const held{uiservice::bundle()};
    CHECK(held.tours->add(resultsTour("computed", R"({"fn":"format","args":["Computed"]})")).has_value());
}

TEST_CASE("tours edges: replace refuses a tour that fails the checks and keeps the registered one",
          "[ui][tours][runtime]") {
    Held const held{uiservice::bundle()};
    int changes = 0;
    held.tours->setOnChanged([&changes] { ++changes; });
    REQUIRE(held.tours->add(resultsTour("kept")).has_value());
    auto const refused =
        held.tours->replace(R"({"id":"kept","screen":"lims.nope","title":"T","steps":[{"title":"S"}]})");
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().tourId == "kept");
    CHECK(refused.error().path == "/screen");
    CHECK(changes == 1);
    morph::session::Context const nobody;
    auto const listed = held.tours->listFor(nobody, uiservice::RuleAuthorizer{});
    REQUIRE(listed.tours.size() == 1);
    CHECK(listed.tours.front().revision == 1);
}

TEST_CASE("tours edges: a tour's text keys are read from the bundle's catalogues and the server's texts",
          "[ui][tours][runtime]") {
    std::vector<std::string> asked;
    auto registry = catalogued({"en", "de"});
    registry.setFieldTexts([&asked](std::string_view locale) {
        asked.emplace_back(locale);
        ui::FieldTexts texts;
        if (locale == "en") {
            texts.emplace("tours.server.title", "Server title");
        }
        return texts;
    });
    Held const held{built(registry)};
    // A key only the server's texts have, and one the catalogues have.
    CHECK(held.tours->add(resultsTour("served", textTitle("tours.server.title"))).has_value());
    CHECK(held.tours->add(resultsTour("catalogued", textTitle("tours.new.title"))).has_value());
    auto const unknown = held.tours->add(resultsTour("unknown", textTitle("tours.nowhere")));
    REQUIRE_FALSE(unknown.has_value());
    CHECK(unknown.error().path == "/title");
    CHECK(unknown.error().reason.contains("`tours.nowhere`"));
    // Every locale the catalogues are for is asked.
    CHECK(std::ranges::count(asked, "de") >= 1);
    CHECK(std::ranges::count(asked, "en") >= 1);
}

TEST_CASE("tours edges: without a catalogue the server's texts are read in English", "[ui][tours][runtime]") {
    auto const asked = std::make_shared<std::vector<std::string>>();
    ui::UiRegistry registry = uiservice::registry();
    registry.setFieldTexts([asked](std::string_view locale) {
        asked->emplace_back(locale);
        return ui::FieldTexts{{"tours.server.title", "Server title"}};
    });
    Held const held{built(registry)};
    CHECK(held.tours->add(resultsTour("served", textTitle("tours.server.title"))).has_value());
    CHECK(*asked == std::vector<std::string>{"en"});
}

TEST_CASE("tours edges: a server text source that throws is logged and its keys are unknown", "[ui][tours][runtime]") {
    LogCapture const log;
    ui::UiRegistry registry = uiservice::registry();
    registry.setFieldTexts(
        [](std::string_view /*locale*/) -> ui::FieldTexts { throw std::runtime_error{"no texts"}; });
    Held const held{built(registry)};
    auto const refused = held.tours->add(resultsTour("served", textTitle("tours.server.title")));
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().reason.contains("`tours.server.title`"));
    CHECK(log.saw(LogLevel::warn, "the server's texts for `en` could not be read: no texts"));
}

TEST_CASE("tours edges: a tour whose screen the served bundle no longer has is not listed", "[ui][tours][runtime]") {
    Held held{uiservice::bundle()};
    REQUIRE(held.tours->add(resultsTour("on-results")).has_value());
    // The same digest, so the registry does not check its tours again, but without the screen.
    auto reduced = std::make_shared<ui::ServedBundle>(*held.bundle);
    std::erase_if(reduced->screens, [](ui::ServedScreen const& screen) { return screen.entry.id == "lims.results"; });
    held.bundle = std::move(reduced);
    morph::session::Context const nobody;
    CHECK(held.tours->listFor(nobody, uiservice::RuleAuthorizer{}).tours.empty());
}

TEST_CASE("tours edges: a caller with no session lists over a connection that never signed in",
          "[ui][tours][runtime]") {
    morph::model::detail::ActionDispatcher dispatcher;
    morph::model::detail::ModelRegistryFactory registry;
    morph::exec::ThreadPoolExecutor pool{2};
    auto const authorizer = std::make_shared<uiservice::RuleAuthorizer>();
    Held const held{uiservice::bundle()};
    ui::registerTours(held.tours, authorizer, dispatcher, registry);
    auto const server = std::make_shared<morph::backend::RemoteServer>(pool, authorizer, dispatcher, registry);
    REQUIRE(held.tours->add(resultsTour("open-to-all")).has_value());
    morph::testing::StepExecutor owner;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::SimulatedRemoteBackend>(*server), owner};
    ui::BridgeDispatcher dispatcherOfClient{bridge, &owner};
    ui::TourSync sync{dispatcherOfClient};
    std::optional<bool> offered;
    sync.list([&offered](bool answer) { offered = answer; });
    REQUIRE(morph::testing::waitUntil([&] {
        owner.runAll();
        return offered.has_value();
    }));
    CHECK(*offered);
    REQUIRE(sync.tours().size() == 1);
    CHECK(sync.tours().front().id == "open-to-all");
}

TEST_CASE("tours edges: a published change after the server is gone does nothing", "[ui][tours][runtime]") {
    Held const held{uiservice::bundle()};
    {
        morph::exec::ThreadPoolExecutor pool{1};
        auto const authorizer = std::make_shared<uiservice::RuleAuthorizer>();
        morph::model::detail::ActionDispatcher dispatcher;
        morph::model::detail::ModelRegistryFactory registry;
        auto const server = std::make_shared<morph::backend::RemoteServer>(pool, authorizer, dispatcher, registry);
        ui::publishTourChanges(*held.tours, server);
    }
    CHECK(held.tours->add(resultsTour("after-the-server")).has_value());
    CHECK(held.tours->remove("after-the-server").has_value());
}

// ---------------------------------------------------------------------------------------------------------------
// The bundle checks.
// ---------------------------------------------------------------------------------------------------------------

namespace {

// A screen with a literal test id, and a member holding an object whose `testId` is not text.
constexpr std::string_view kSamples = R"({"vocab":"ui/1","id":"lims.samples","title":"Samples",
  "state":{"n":{"init":0}},"note":{"testId":7},
  "view":{"kind":"column","children":[{"kind":"text","testId":"samples_table","text":"rows"}]}})";

std::string homeScreen(std::string_view args) {
    return R"({"vocab":"ui/1","id":"lims.home","title":"Home","state":{"which":{"init":"basics"}},
      "view":{"kind":"button","testId":"take_tour","label":"Take the tour",
              "onClick":[{"host":"tour.start","args":)" +
           std::string{args} + "}]}}";
}

/// Every part, over the corpus catalog's actions, for the modules `lab` and `billing`.
ui::ShellParts allParts() {
    ui::ShellParts parts;
    parts.context = ui::ContextSwitch{.model = "Lims_ClientModel",
                                      .action = "Lims_ClientScopes",
                                      .localeField = "lang",
                                      .optionsField = "options",
                                      .labelField = "label",
                                      .valueField = "value",
                                      .detailField = "detail",
                                      .groupField = "group"};
    parts.search = {ui::SearchSource{.module = "lab",
                                     .title = "Lab",
                                     .model = "Lims_ClientModel",
                                     .action = "Lims_ClientSearchLab",
                                     .queryField = "q",
                                     .resultsField = "hits",
                                     .contextField = "scope",
                                     .localeField = {}}};
    parts.inbox = ui::InboxEntry{.screen = "lims.home",
                                 .sources = {ui::ModuleCount{.module = "billing",
                                                             .model = "Lims_ClientModel",
                                                             .action = "Lims_ClientApprovalsBilling",
                                                             .countField = "n",
                                                             .contextField = {},
                                                             .localeField = {}}}};
    parts.principal = ui::PrincipalMenu{.model = "Lims_ClientModel",
                                        .action = "Lims_ClientPerson",
                                        .contextField = {},
                                        .localeField = {},
                                        .nameField = "name",
                                        .detailField = "detail",
                                        .subtitleField = "role",
                                        .rolesField = "roles",
                                        .locales = {"en"},
                                        .formats = {"en"}};
    parts.brand = ui::BrandMark{.title = "Lims", .icon = {}, .version = {}};
    parts.help = ui::HelpEntry{.screen = "lims.home", .model = {}, .action = {}, .params = {}};
    return parts;
}

/// A bundle of two screens, a public one, a host screen and an app document with @p tours and @p members, built over
/// @p parts.
std::expected<std::shared_ptr<ui::ServedBundle const>, ui::Refusal> buildChecked(
    std::string const& tours, std::optional<ui::ShellParts> const& parts = std::nullopt,
    std::string const& members = {}, std::string_view startArgs = R"({"obj":{"id":"basics"}})") {
    ui::UiRegistry registry{ui::ApplicationInfo{.id = "lims", .version = "1"}, ui::testing::corpusCatalog()};
    auto const add = [&registry](std::string id, std::string module, std::string text) {
        ui::CatalogEntry entry;
        entry.id = std::move(id);
        entry.title = "Screen";
        entry.module = std::move(module);
        auto const added = registry.addScreen(std::move(entry), std::move(text));
        REQUIRE(added.has_value());
    };
    add("lims.samples", "lab", std::string{kSamples});
    add("lims.home", "billing", homeScreen(startArgs));
    auto const login = [] {
        ui::CatalogEntry entry;
        entry.id = "lims.login";
        entry.title = "Sign in";
        entry.isPublic = true;
        return entry;
    };
    REQUIRE(registry.addScreen(login(), R"({"vocab":"ui/1","id":"lims.login","view":{"kind":"text","text":"x"}})")
                .has_value());
    auto const explorer = [] {
        ui::CatalogEntry entry;
        entry.id = "explorer";
        entry.title = "Explorer";
        entry.host = std::string{"explorer"};
        return entry;
    };
    REQUIRE(registry.addEntry(explorer()).has_value());
    auto const app = registry.addFile(ui::BundleFile{
        .kind = "app", .id = "app", .bytes = R"({"vocab":"ui/1","id":"app","tours":)" + tours + members + "}"});
    REQUIRE(app.has_value());
    if (parts) {
        REQUIRE(registry.setShellParts(*parts, ui::testing::corpusCatalog()).has_value());
    }
    return registry.build();
}

/// A tour `basics` of one step ringing @p target.
std::string ringing(std::string_view target) {
    return R"([{"id":"basics","title":"T","steps":[{"target":")" + std::string{target} + R"(","title":"S"}]}])";
}

}  // namespace

TEST_CASE("tours edges: a shell id is accepted once the app declares its part, and refused when it does not",
          "[ui][tours][service]") {
    ui::ShellParts partial;
    partial.brand = ui::BrandMark{.title = "Lims", .icon = {}, .version = {}};
    partial.help = ui::HelpEntry{.screen = "lims.home", .model = {}, .action = {}, .params = {}};
    for (std::string_view const target : {"shell_search", "shell_inbox", "shell_help", "shell_principal_menu",
                                          "shell_brand_title", "shell_context", "shell_tours", "chat_input"}) {
        INFO(target);
        auto const full = buildChecked(ringing(target), allParts());
        INFO((full ? std::string{"built"} : full.error().message()));
        CHECK(full.has_value());
    }
    for (std::string_view const target : {"shell_search", "shell_inbox", "shell_principal_menu", "shell_context"}) {
        INFO(target);
        auto const refused = buildChecked(ringing(target), partial);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().path == "/tours/0/steps/0/target");
        CHECK(refused.error().reason.contains(target));
    }
    for (std::string_view const target : {"shell_brand_title", "shell_help"}) {
        INFO(target);
        CHECK(buildChecked(ringing(target), partial).has_value());
    }
    // An id of the shell that no part is named for is the app's to get wrong, whatever it declares.
    CHECK_FALSE(buildChecked(ringing("shell_unheard_of"), allParts()).has_value());
}

TEST_CASE("tours edges: an app document without a shell refuses a shell part's id", "[ui][tours][service]") {
    for (std::string_view const target : {"shell_search", "shell_brand_title"}) {
        INFO(target);
        auto const refused = buildChecked(ringing(target));
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().path == "/tours/0/steps/0/target");
    }
    CHECK(buildChecked(ringing("shell_tours")).has_value());
}

TEST_CASE("tours edges: a nav id is accepted for a listed screen with a document only", "[ui][tours][service]") {
    CHECK(buildChecked(ringing("nav_lims.samples")).has_value());
    for (std::string_view const target : {"nav_lims.nowhere", "nav_lims.login", "nav_explorer"}) {
        INFO(target);
        auto const refused = buildChecked(ringing(target));
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().path == "/tours/0/steps/0/target");
    }
}

TEST_CASE("tours edges: a tour.start whose tour is computed or whose record is not literal is not refused",
          "[ui][tours][service]") {
    std::string const notes =
        R"(,"notes":[
        {"host":"tour.start","args":5},
        {"host":"tour.start","args":{"ref":"which"}},
        {"host":"tour.start","args":{"obj":{"id":"basics"},"extra":1}},
        {"host":"tour.start","args":{"obj":7}},
        {"host":"tour.start","args":{"obj":{"name":"basics"}}},
        {"host":"tour.start","args":{"obj":{"id":5}}},
        {"host":7,"args":{"obj":{"id":"nope"}}},
        {"host":"other.command","args":{"obj":{"id":"nope"}}},
        {"args":{"obj":{"id":"nope"}}},
        {"host":"tour.start"}])";
    auto const accepted = buildChecked(ringing("shell_tours"), std::nullopt, notes);
    INFO((accepted ? std::string{"built"} : accepted.error().message()));
    CHECK(accepted.has_value());
    // The same shapes with a literal undeclared id in the last note: that one is refused, naming its place.
    std::string const withLiteral =
        notes.substr(0, notes.size() - 1) + R"(,{"host":"tour.start","args":{"lit":{"id":"nope"}}}])";
    auto const refused = buildChecked(ringing("shell_tours"), std::nullopt, withLiteral);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().document == "app");
    CHECK(refused.error().path == "/notes/10");
    CHECK(refused.error().reason.contains("`nope`"));
}

// ---------------------------------------------------------------------------------------------------------------
// The controller and the card.
// ---------------------------------------------------------------------------------------------------------------

namespace {

/// A shell a tour runs in, over a `RecordingBackend`, whose texts a test can set.
class EdgeShell final : public ui::TourShell {
public:
    explicit EdgeShell(ui::testing::RecordingBackend& backend) : _backend{&backend} {}

    std::expected<void, ui::MountError> openScreen(std::string const& screenId, ui::ParamMap const& params) override {
        if (refused.contains(screenId)) {
            return std::unexpected(
                ui::MountError{.screenId = screenId, .reason = "not for this person", .pointer = {}});
        }
        active = screenId;
        activeParams = params;
        return {};
    }
    [[nodiscard]] bool shows(std::string const& screenId, ui::ParamMap const& params) const override {
        return active == screenId && activeParams == params;
    }
    [[nodiscard]] std::string activeScreen() const override { return active; }
    [[nodiscard]] std::string screenTitle(std::string const& screenId) const override {
        return "Title of " + screenId;
    }
    [[nodiscard]] bool compact() const override { return false; }
    void revealNavEntry(std::string const& /*screenId*/) override {}
    ui::SpotlightWidget& spotlight() override {
        if (_spotlight == nullptr) {
            _spotlight = _backend->createSpotlight(nullptr);
        }
        return *_spotlight;
    }
    void notify(std::string const& text, ui::Tone tone) override { notices.emplace_back(text, tone); }
    [[nodiscard]] std::string text(std::string_view key) const override {
        if (auto const found = texts.find(key); found != texts.end()) {
            return found->second;
        }
        return ui::shellText(key, ui::FormatContext{});
    }
    void afterCardUpdated(std::function<void()> then) override {
        if (deferCard) {
            pending.push_back(std::move(then));
            return;
        }
        then();
    }

    /// The spotlight's widget id; 0 before the first tour.
    [[nodiscard]] int spotlightId() const { return _spotlight == nullptr ? 0 : _backend->idOf(*_spotlight); }

    // NOLINTBEGIN(cppcoreguidelines-non-private-member-variables-in-classes): what a test sets and reads.
    std::string active;
    ui::ParamMap activeParams;
    std::set<std::string, std::less<>> refused;
    std::map<std::string, std::string, std::less<>> texts;
    std::vector<std::pair<std::string, ui::Tone>> notices;
    bool deferCard = false;
    std::vector<std::function<void()>> pending;
    // NOLINTEND(cppcoreguidelines-non-private-member-variables-in-classes)

private:
    ui::testing::RecordingBackend* _backend;
    std::unique_ptr<ui::SpotlightWidget> _spotlight;
};

constexpr std::string_view kApp = R"({
  "vocab": "ui/1", "id": "app",
  "tours": [
    { "id": "general", "title": "General", "steps": [ { "title": "One" }, { "title": "Two" } ] },
    { "id": "on-samples", "screen": "samples", "title": "On samples", "steps": [ { "title": "A" } ] },
    { "id": "waits-last", "title": "Waits",
      "steps": [ { "title": "A" }, { "advanceOn": { "screen": "samples" }, "title": "Go" } ] },
    { "id": "targeted", "title": "Targeted",
      "steps": [ { "target": "shell_search", "title": "Search" }, { "title": "Nothing ringed" } ] } ] })";

/// A runtime tour @p id at @p revision with one step.
ui::TourDecl runtimeTour(std::string const& id, std::uint64_t revision) {
    auto loaded = ui::loadTourText(R"({"id":")" + id + R"(","title":"Runtime","steps":[{"title":"Look"}]})");
    REQUIRE(loaded.has_value());
    loaded->revision = revision;
    return std::move(*loaded);
}

struct Rig {
    Rig() {
        auto mounted = h.interpreter.mountApp(ui::testing::loadOrFail(kApp));
        REQUIRE(mounted.has_value());
        root = std::move(*mounted);
        page = h.backend.createStack(nullptr, ui::Axis::Vertical);
        controller = std::make_unique<ui::TourController>(
            h.runtime, *root, shell,
            ui::TourControllerOptions{.warn = [this](std::string const& text) { warnings.push_back(text); },
                                      .onChanged = [this] { ++changes; }});
    }

    /// Puts a widget carrying @p testId on the page.
    void carry(std::string const& testId) {
        parts.push_back(h.backend.createButton(page.get()));
        parts.back()->setTestId(testId);
    }
    [[nodiscard]] std::string spot(std::string_view name) const { return h.backend.prop(shell.spotlightId(), name); }

    ui::testing::Harness h;
    EdgeShell shell{h.backend};
    std::unique_ptr<ui::AppRoot> root;
    std::unique_ptr<ui::StackWidget> page;
    std::vector<std::unique_ptr<ui::ButtonWidget>> parts;
    std::unique_ptr<ui::TourController> controller;
    std::vector<std::string> warnings;
    int changes = 0;
};

}  // namespace

TEST_CASE("tours edges: a tour is offered on its own screen and a general tour everywhere", "[ui][tours]") {
    std::vector<ui::Tour> const tours{
        ui::Tour{.id = "general", .screen = {}, .title = "G", .summary = {}, .steps = {}, .revision = 0},
        ui::Tour{.id = "mine", .screen = "samples", .title = "M", .summary = {}, .steps = {}, .revision = 0},
        ui::Tour{.id = "other", .screen = "home", .title = "O", .summary = {}, .steps = {}, .revision = 0}};
    ui::OfferedTours const onSamples = ui::offeredTours(tours, "samples");
    REQUIRE(onSamples.screen.size() == 1);
    CHECK(onSamples.screen.front().id == "mine");
    REQUIRE(onSamples.general.size() == 1);
    CHECK(onSamples.general.front().id == "general");
    // No active screen: only the general tour; a screen with no tours of its own: the same.
    CHECK(ui::offeredTours(tours, "").screen.empty());
    CHECK(ui::offeredTours(tours, "").general.size() == 1);
    CHECK(ui::offeredTours(tours, "nowhere").screen.empty());
}

TEST_CASE("tours edges: a tour whose own screen cannot be opened ends at once with a warning and a toast",
          "[ui][tours]") {
    Rig r;
    r.shell.refused.insert("samples");
    CHECK(r.controller->start("on-samples").has_value());
    CHECK_FALSE(r.controller->running());
    REQUIRE(r.shell.notices.size() == 1);
    CHECK(r.shell.notices.front().second == ui::Tone::Warn);
    CHECK(r.shell.notices.front().first.contains("Title of samples"));
    REQUIRE(r.warnings.size() == 1);
    CHECK(r.warnings.front().contains("the tour `on-samples` stopped"));
    CHECK(r.warnings.front().contains("not for this person"));
}

TEST_CASE("tours edges: Back does nothing with no tour or on the first step", "[ui][tours]") {
    Rig r;
    r.controller->back();
    CHECK(r.changes == 0);
    REQUIRE(r.controller->start("general").has_value());
    int const before = r.changes;
    r.controller->back();
    CHECK(r.changes == before);
    CHECK(r.controller->view()->position == 1);
}

TEST_CASE("tours edges: a change of screen is not a move on unless the step waits for it", "[ui][tours]") {
    Rig r;
    REQUIRE(r.controller->start("general").has_value());
    r.shell.active = "samples";
    r.controller->activeChanged();
    CHECK(r.controller->view()->position == 1);
    r.controller->end();
    // A waiting last step: the screen it waits for ends the tour.
    REQUIRE(r.controller->start("waits-last").has_value());
    r.shell.active = "";
    r.controller->next();
    REQUIRE(r.controller->view().has_value());
    REQUIRE(r.controller->view()->waiting);
    r.shell.active = "samples";
    r.controller->activeChanged();
    CHECK_FALSE(r.controller->running());
}

TEST_CASE("tours edges: a form change with no tour touches nothing, and with one rings the step's target again",
          "[ui][tours]") {
    Rig r;
    r.controller->formChanged();
    CHECK(r.shell.spotlightId() == 0);
    REQUIRE(r.controller->start("targeted").has_value());
    CHECK(r.spot("targetShown") == "false");
    r.carry("shell_search");
    r.controller->formChanged();
    CHECK(r.spot("targetShown") == "true");
    r.controller->next();
    r.controller->formChanged();
    CHECK(r.spot("targetShown") == "false");
}

TEST_CASE("tours edges: a toast text with no title placeholder is shown as it is", "[ui][tours]") {
    Rig r;
    r.shell.texts["morph.shell.tourChanged"] = "This tour changed.";
    r.root->setRuntimeTours({runtimeTour("whats-new", 1)});
    r.h.settle();
    REQUIRE(r.controller->start("whats-new").has_value());
    r.root->setRuntimeTours({runtimeTour("whats-new", 2)});
    r.h.settle();
    CHECK_FALSE(r.controller->running());
    REQUIRE(r.shell.notices.size() == 1);
    CHECK(r.shell.notices.front().first == "This tour changed.");
}

TEST_CASE("tours edges: a change of the tours after the tour ended says nothing", "[ui][tours]") {
    Rig r;
    r.root->setRuntimeTours({runtimeTour("whats-new", 1)});
    r.h.settle();
    REQUIRE(r.controller->start("whats-new").has_value());
    r.controller->end();
    int const before = r.changes;
    r.root->setRuntimeTours({runtimeTour("whats-new", 2)});
    r.h.settle();
    CHECK(r.shell.notices.empty());
    CHECK(r.changes == before);
    CHECK_FALSE(r.controller->running());
}

TEST_CASE("tours edges: the card opens only for the start whose update it waited for, and only while it runs",
          "[ui][tours]") {
    SECTION("a tour that ended before the card updated") {
        Rig r;
        r.shell.deferCard = true;
        REQUIRE(r.controller->start("general").has_value());
        REQUIRE(r.shell.pending.size() == 1);
        r.controller->end();
        r.shell.pending.front()();
        CHECK(r.spot("open") != "true");
    }
    SECTION("a start replaced by another before the card updated") {
        Rig r;
        r.shell.deferCard = true;
        REQUIRE(r.controller->start("general").has_value());
        REQUIRE(r.controller->start("general").has_value());
        REQUIRE(r.shell.pending.size() == 2);
        r.shell.pending.front()();
        CHECK(r.spot("open") != "true");
        r.shell.pending.back()();
        CHECK(r.spot("open") == "true");
    }
    SECTION("a controller that is gone before the card updated") {
        Rig r;
        r.shell.deferCard = true;
        REQUIRE(r.controller->start("general").has_value());
        REQUIRE(r.shell.pending.size() == 1);
        r.controller.reset();
        r.shell.pending.front()();
        CHECK(r.spot("open") != "true");
    }
}

namespace {

/// A card binding over a harness, a controller and a shell, with the post and the report a test chooses.
struct CardRig {
    explicit CardRig(std::function<void(std::function<void()>)> post = {},
                     std::function<void(std::string const&)> report = {}) {
        auto mounted = h.interpreter.mountApp(ui::testing::loadOrFail(kApp));
        REQUIRE(mounted.has_value());
        root = std::move(*mounted);
        binding = std::make_unique<ui::TourCardBinding>(
            host, [this] { return controller.get(); }, std::move(post), std::move(report));
        controller = std::make_unique<ui::TourController>(
            h.runtime, *root, shell,
            ui::TourControllerOptions{.warn = {}, .onChanged = [this] { binding->refresh(); }});
    }

    [[nodiscard]] ui::HostCommandResult run(std::string const& name, std::string const& tourId = {}) const {
        if (tourId.empty()) {
            return host.run(name, ui::Value{});
        }
        return host.run(name, ui::Value{ui::ValueRecord{{"id", ui::Value{tourId}}}});
    }

    ui::HostVocabulary host;
    ui::testing::Harness h;
    EdgeShell shell{h.backend};
    std::unique_ptr<ui::AppRoot> root;
    std::unique_ptr<ui::TourCardBinding> binding;
    std::unique_ptr<ui::TourController> controller;
};

}  // namespace

TEST_CASE("tours edges: a card binding with no controller changes nothing and refuses tour.start",
          "[ui][tours][card]") {
    ui::HostVocabulary host;
    ui::TourCardBinding const binding{host, {}};
    CHECK(host.run("tour.next", ui::Value{}).kind == ui::HostCommandResult::Kind::Ran);
    CHECK(host.run("tour.back", ui::Value{}).kind == ui::HostCommandResult::Kind::Ran);
    CHECK(host.run("tour.end", ui::Value{}).kind == ui::HostCommandResult::Kind::Ran);
    auto const started = host.run("tour.start", ui::Value{ui::ValueRecord{{"id", ui::Value{"general"}}}});
    CHECK(started.kind == ui::HostCommandResult::Kind::Refused);
    CHECK(started.reason.contains("no frame is shown"));
}

TEST_CASE("tours edges: a card binding with no post runs its commands at once", "[ui][tours][card]") {
    CardRig r;
    CHECK(r.run("tour.start", "general").kind == ui::HostCommandResult::Kind::Ran);
    REQUIRE(r.controller->running());
    CHECK(r.run("tour.next").kind == ui::HostCommandResult::Kind::Ran);
    CHECK(r.controller->view()->position == 2);
    CHECK(r.run("tour.back").kind == ui::HostCommandResult::Kind::Ran);
    CHECK(r.controller->view()->position == 1);
    CHECK(r.run("tour.end").kind == ui::HostCommandResult::Kind::Ran);
    CHECK_FALSE(r.controller->running());
    auto const unknown = r.run("tour.start", "nope");
    CHECK(unknown.kind == ui::HostCommandResult::Kind::Refused);
    CHECK(unknown.reason.contains("offers no tour `nope`"));
}

TEST_CASE("tours edges: after the card binding is gone its commands do nothing and tour.start is refused",
          "[ui][tours][card]") {
    CardRig r;
    REQUIRE(r.run("tour.start", "general").kind == ui::HostCommandResult::Kind::Ran);
    r.binding.reset();
    CHECK(r.run("tour.next").kind == ui::HostCommandResult::Kind::Ran);
    CHECK(r.controller->view()->position == 1);
    auto const started = r.run("tour.start", "waits-last");
    CHECK(started.kind == ui::HostCommandResult::Kind::Refused);
    CHECK(started.reason.contains("the window is gone"));
    CHECK(r.controller->view()->tourId == "general");
}

TEST_CASE("tours edges: a posted tour.start whose frame is gone and that has no report callback warns",
          "[ui][tours][card]") {
    LogCapture const log;
    std::vector<std::function<void()>> posted;
    CardRig r{[&posted](std::function<void()> work) { posted.push_back(std::move(work)); }};
    REQUIRE(r.run("tour.start", "general").kind == ui::HostCommandResult::Kind::Ran);
    REQUIRE(posted.size() == 1);
    auto gone = std::move(r.controller);
    posted.front()();
    CHECK_FALSE(gone->running());
    CHECK(log.saw(LogLevel::warn, "the tour `general` did not start: no frame is shown"));
    r.controller = std::move(gone);
}

TEST_CASE("tours edges: authoring writes a tour's screen, and refuses an app document that is not JSON",
          "[ui][tours][author]") {
    namespace author = ui::author;
    std::vector<author::Tour> const tours{author::Tour{.id = "on-samples",
                                                       .screen = "samples",
                                                       .title = "Samples",
                                                       .summary = {},
                                                       .steps = {author::TourStep{.title = "Here"}}}};
    auto const written = author::withTours(R"({"vocab":"ui/1","id":"app"})", tours);
    REQUIRE(written.has_value());
    auto const loaded = ui::loadDocumentText(*written);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->tours.size() == 1);
    CHECK(loaded->tours.front().screen == "samples");
    auto const refused = author::withTours("not json at all", tours);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().reason.contains("JSON object"));
}

TEST_CASE("tours edges: a tour id may hold digits, and a step's param must be a literal", "[ui][tours]") {
    CHECK(ui::loadTourText(R"({"id":"v2-tour","title":"T","steps":[{"title":"S"}]})").has_value());
    auto const list = ui::loadTourText(R"({"id":"a","title":"T","steps":[{"title":"S","params":{"p":[1]}}]})");
    REQUIRE_FALSE(list.has_value());
    CHECK(list.error().path == "/steps/0/params/p");
    auto const fraction = ui::loadTourText(R"({"id":"a","title":"T","steps":[{"title":"S","params":{"p":1.5}}]})");
    INFO((fraction ? std::string{"loaded"} : fraction.error().reason));
    CHECK(fraction.has_value() == false);
}
